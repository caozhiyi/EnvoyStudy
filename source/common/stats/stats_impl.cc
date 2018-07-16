#include "common/stats/stats_impl.h"

#include <string.h>

#include <algorithm>
#include <chrono>
#include <string>

#include "envoy/common/exception.h"

#include "common/common/lock_guard.h"
#include "common/common/perf_annotation.h"
#include "common/common/thread.h"
#include "common/common/utility.h"
#include "common/config/well_known_names.h"

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"

namespace Envoy {
namespace Stats {

namespace {

// Round val up to the next multiple of the natural alignment.
// Note: this implementation only works because 8 is a power of 2.
uint64_t roundUpMultipleNaturalAlignment(uint64_t val) {
  const uint64_t multiple = alignof(RawStatData);
  static_assert(multiple == 1 || multiple == 2 || multiple == 4 || multiple == 8 || multiple == 16,
                "multiple must be a power of 2 for this algorithm to work");
  return (val + multiple - 1) & ~(multiple - 1);
}

bool regexStartsWithDot(absl::string_view regex) {
  return absl::StartsWith(regex, "\\.") || absl::StartsWith(regex, "(?=\\.)");
}

} // namespace

// Normally the compiler would do this, but because name_ is a flexible-array-length
// element, the compiler can't. RawStatData is put into an array in HotRestartImpl, so
// it's important that each element starts on the required alignment for the type.
uint64_t RawStatData::structSize(uint64_t name_size) {
  return roundUpMultipleNaturalAlignment(sizeof(RawStatData) + name_size + 1);
}

uint64_t RawStatData::structSizeWithOptions(const StatsOptions& stats_options) {
  return structSize(stats_options.maxNameLength());
}

std::string Utility::sanitizeStatsName(const std::string& name) {
  std::string stats_name = name;
  std::replace(stats_name.begin(), stats_name.end(), ':', '_');
  std::replace(stats_name.begin(), stats_name.end(), '\0', '_');
  return stats_name;
}

TagExtractorImpl::TagExtractorImpl(const std::string& name, const std::string& regex,
                                   const std::string& substr)
    : name_(name), prefix_(std::string(extractRegexPrefix(regex))), substr_(substr),
      regex_(RegexUtil::parseRegex(regex)) {}

std::string TagExtractorImpl::extractRegexPrefix(absl::string_view regex) {
  std::string prefix;
  if (absl::StartsWith(regex, "^")) {
    for (absl::string_view::size_type i = 1; i < regex.size(); ++i) {
      if (!absl::ascii_isalnum(regex[i]) && (regex[i] != '_')) {
        if (i > 1) {
          const bool last_char = i == regex.size() - 1;
          if ((!last_char && regexStartsWithDot(regex.substr(i))) ||
              (last_char && (regex[i] == '$'))) {
            prefix.append(regex.data() + 1, i - 1);
          }
        }
        break;
      }
    }
  }
  return prefix;
}

TagExtractorPtr TagExtractorImpl::createTagExtractor(const std::string& name,
                                                     const std::string& regex,
                                                     const std::string& substr) {

  if (name.empty()) {
    throw EnvoyException("tag_name cannot be empty");
  }

  if (regex.empty()) {
    throw EnvoyException(fmt::format(
        "No regex specified for tag specifier and no default regex for name: '{}'", name));
  }
  return TagExtractorPtr{new TagExtractorImpl(name, regex, substr)};
}

bool TagExtractorImpl::substrMismatch(const std::string& stat_name) const {
  return !substr_.empty() && stat_name.find(substr_) == std::string::npos;
}

bool TagExtractorImpl::extractTag(const std::string& stat_name, std::vector<Tag>& tags,
                                  IntervalSet<size_t>& remove_characters) const {
  PERF_OPERATION(perf);

  if (substrMismatch(stat_name)) {
    PERF_RECORD(perf, "re-skip-substr", name_);
    return false;
  }

  std::smatch match;
  // The regex must match and contain one or more subexpressions (all after the first are ignored).
  if (std::regex_search(stat_name, match, regex_) && match.size() > 1) {
    // remove_subexpr is the first submatch. It represents the portion of the string to be removed.
    const auto& remove_subexpr = match[1];

    // value_subexpr is the optional second submatch. It is usually inside the first submatch
    // (remove_subexpr) to allow the expression to strip off extra characters that should be removed
    // from the string but also not necessary in the tag value ("." for example). If there is no
    // second submatch, then the value_subexpr is the same as the remove_subexpr.
    const auto& value_subexpr = match.size() > 2 ? match[2] : remove_subexpr;

    tags.emplace_back();
    Tag& tag = tags.back();
    tag.name_ = name_;
    tag.value_ = value_subexpr.str();

    // Determines which characters to remove from stat_name to elide remove_subexpr.
    std::string::size_type start = remove_subexpr.first - stat_name.begin();
    std::string::size_type end = remove_subexpr.second - stat_name.begin();
    remove_characters.insert(start, end);
    PERF_RECORD(perf, "re-match", name_);
    return true;
  }
  PERF_RECORD(perf, "re-miss", name_);
  return false;
}

RawStatData* HeapRawStatDataAllocator::alloc(const std::string& name) {
  uint64_t num_bytes_to_allocate = RawStatData::structSize(name.size());
  RawStatData* data = static_cast<RawStatData*>(::calloc(num_bytes_to_allocate, 1));
  if (data == nullptr) {
    throw std::bad_alloc();
  }
  data->checkAndInit(name, num_bytes_to_allocate);

  Thread::ReleasableLockGuard lock(mutex_);
  auto ret = stats_.insert(data);
  RawStatData* existing_data = *ret.first;
  lock.release();

  if (!ret.second) {
    ::free(data);
    ++existing_data->ref_count_;
    return existing_data;
  } else {
    return data;
  }
}

/**
 * Counter implementation that wraps a RawStatData.
 */
class CounterImpl : public Counter, public MetricImpl {
public:
  CounterImpl(RawStatData& data, RawStatDataAllocator& alloc, std::string&& tag_extracted_name,
              std::vector<Tag>&& tags)
      : MetricImpl(data.name_, std::move(tag_extracted_name), std::move(tags)), data_(data),
        alloc_(alloc) {}
  ~CounterImpl() { alloc_.free(data_); }

  // Stats::Counter
  void add(uint64_t amount) override {
    data_.value_ += amount;
    data_.pending_increment_ += amount;
    data_.flags_ |= Flags::Used;
  }

  void inc() override { add(1); }
  uint64_t latch() override { return data_.pending_increment_.exchange(0); }
  void reset() override { data_.value_ = 0; }
  bool used() const override { return data_.flags_ & Flags::Used; }
  uint64_t value() const override { return data_.value_; }

private:
  RawStatData& data_;
  RawStatDataAllocator& alloc_;
};

/**
 * Gauge implementation that wraps a RawStatData.
 */
class GaugeImpl : public Gauge, public MetricImpl {
public:
  GaugeImpl(RawStatData& data, RawStatDataAllocator& alloc, std::string&& tag_extracted_name,
            std::vector<Tag>&& tags)
      : MetricImpl(data.name_, std::move(tag_extracted_name), std::move(tags)), data_(data),
        alloc_(alloc) {}
  ~GaugeImpl() { alloc_.free(data_); }

  // Stats::Gauge
  virtual void add(uint64_t amount) override {
    data_.value_ += amount;
    data_.flags_ |= Flags::Used;
  }
  virtual void dec() override { sub(1); }
  virtual void inc() override { add(1); }
  virtual void set(uint64_t value) override {
    data_.value_ = value;
    data_.flags_ |= Flags::Used;
  }
  virtual void sub(uint64_t amount) override {
    ASSERT(data_.value_ >= amount);
    ASSERT(used());
    data_.value_ -= amount;
  }
  virtual uint64_t value() const override { return data_.value_; }
  bool used() const override { return data_.flags_ & Flags::Used; }

private:
  RawStatData& data_;
  RawStatDataAllocator& alloc_;
};

TagProducerImpl::TagProducerImpl(const envoy::config::metrics::v2::StatsConfig& config) {
  // To check name conflict.
  reserveResources(config);
  std::unordered_set<std::string> names = addDefaultExtractors(config);

  for (const auto& tag_specifier : config.stats_tags()) {
    const std::string& name = tag_specifier.tag_name();
    if (!names.emplace(name).second) {
      throw EnvoyException(fmt::format("Tag name '{}' specified twice.", name));
    }

    // If no tag value is found, fallback to default regex to keep backward compatibility.
    if (tag_specifier.tag_value_case() ==
            envoy::config::metrics::v2::TagSpecifier::TAG_VALUE_NOT_SET ||
        tag_specifier.tag_value_case() == envoy::config::metrics::v2::TagSpecifier::kRegex) {

      if (tag_specifier.regex().empty()) {
        if (addExtractorsMatching(name) == 0) {
          throw EnvoyException(fmt::format(
              "No regex specified for tag specifier and no default regex for name: '{}'", name));
        }
      } else {
        addExtractor(Stats::TagExtractorImpl::createTagExtractor(name, tag_specifier.regex()));
      }
    } else if (tag_specifier.tag_value_case() ==
               envoy::config::metrics::v2::TagSpecifier::kFixedValue) {
      default_tags_.emplace_back(Stats::Tag{.name_ = name, .value_ = tag_specifier.fixed_value()});
    }
  }
}

int TagProducerImpl::addExtractorsMatching(absl::string_view name) {
  int num_found = 0;
  for (const auto& desc : Config::TagNames::get().descriptorVec()) {
    if (desc.name_ == name) {
      addExtractor(
          Stats::TagExtractorImpl::createTagExtractor(desc.name_, desc.regex_, desc.substr_));
      ++num_found;
    }
  }
  return num_found;
}

void TagProducerImpl::addExtractor(TagExtractorPtr extractor) {
  const absl::string_view prefix = extractor->prefixToken();
  if (prefix.empty()) {
    tag_extractors_without_prefix_.emplace_back(std::move(extractor));
  } else {
    tag_extractor_prefix_map_[prefix].emplace_back(std::move(extractor));
  }
}

void TagProducerImpl::forEachExtractorMatching(
    const std::string& stat_name, std::function<void(const TagExtractorPtr&)> f) const {
  IntervalSetImpl<size_t> remove_characters;
  for (const TagExtractorPtr& tag_extractor : tag_extractors_without_prefix_) {
    f(tag_extractor);
  }
  const std::string::size_type dot = stat_name.find('.');
  if (dot != std::string::npos) {
    const absl::string_view token = absl::string_view(stat_name.data(), dot);
    const auto iter = tag_extractor_prefix_map_.find(token);
    if (iter != tag_extractor_prefix_map_.end()) {
      for (const TagExtractorPtr& tag_extractor : iter->second) {
        f(tag_extractor);
      }
    }
  }
}

std::string TagProducerImpl::produceTags(const std::string& metric_name,
                                         std::vector<Tag>& tags) const {
  tags.insert(tags.end(), default_tags_.begin(), default_tags_.end());
  IntervalSetImpl<size_t> remove_characters;
  forEachExtractorMatching(
      metric_name, [&remove_characters, &tags, &metric_name](const TagExtractorPtr& tag_extractor) {
        tag_extractor->extractTag(metric_name, tags, remove_characters);
      });
  return StringUtil::removeCharacters(metric_name, remove_characters);
}

void TagProducerImpl::reserveResources(const envoy::config::metrics::v2::StatsConfig& config) {
  default_tags_.reserve(config.stats_tags().size());
}

std::unordered_set<std::string>
TagProducerImpl::addDefaultExtractors(const envoy::config::metrics::v2::StatsConfig& config) {
  std::unordered_set<std::string> names;
  if (!config.has_use_all_default_tags() || config.use_all_default_tags().value()) {
    for (const auto& desc : Config::TagNames::get().descriptorVec()) {
      names.emplace(desc.name_);
      addExtractor(
          Stats::TagExtractorImpl::createTagExtractor(desc.name_, desc.regex_, desc.substr_));
    }
  }
  return names;
}

void HeapRawStatDataAllocator::free(RawStatData& data) {
  ASSERT(data.ref_count_ > 0);
  if (--data.ref_count_ > 0) {
    return;
  }

  size_t key_removed;
  {
    Thread::LockGuard lock(mutex_);
    key_removed = stats_.erase(&data);
  }

  ASSERT(key_removed == 1);
  ::free(&data);
}

void RawStatData::initialize(absl::string_view key, uint64_t xfer_size) {
  ASSERT(!initialized());
  ref_count_ = 1;
  memcpy(name_, key.data(), xfer_size);
  name_[xfer_size] = '\0';
}

void RawStatData::checkAndInit(absl::string_view key, uint64_t num_bytes_allocated) {
  uint64_t xfer_size = key.size();
  ASSERT(structSize(xfer_size) <= num_bytes_allocated);

  initialize(key, xfer_size);
}

void RawStatData::truncateAndInit(absl::string_view key, const StatsOptions& stats_options) {
  if (key.size() > stats_options.maxNameLength()) {
    ENVOY_LOG_MISC(
        warn,
        "Statistic '{}' is too long with {} characters, it will be truncated to {} characters", key,
        key.size(), stats_options.maxNameLength());
  }

  // key is not necessarily nul-terminated, but we want to make sure name_ is.
  uint64_t xfer_size = std::min(stats_options.maxNameLength(), key.size());
  initialize(key, xfer_size);
}

HistogramStatisticsImpl::HistogramStatisticsImpl(const histogram_t* histogram_ptr)
    : computed_quantiles_(supportedQuantiles().size(), 0.0) {
  hist_approx_quantile(histogram_ptr, supportedQuantiles().data(), supportedQuantiles().size(),
                       computed_quantiles_.data());
}

const std::vector<double>& HistogramStatisticsImpl::supportedQuantiles() const {
  static const std::vector<double> supported_quantiles = {0,    0.25, 0.5,   0.75, 0.90,
                                                          0.95, 0.99, 0.999, 1};
  return supported_quantiles;
}

std::string HistogramStatisticsImpl::summary() const {
  std::vector<std::string> summary;
  const std::vector<double>& supported_quantiles_ref = supportedQuantiles();
  summary.reserve(supported_quantiles_ref.size());
  for (size_t i = 0; i < supported_quantiles_ref.size(); ++i) {
    summary.push_back(
        fmt::format("P{}: {}", 100 * supported_quantiles_ref[i], computed_quantiles_[i]));
  }
  return absl::StrJoin(summary, ", ");
}

/**
 * Clears the old computed values and refreshes it with values computed from passed histogram.
 */
void HistogramStatisticsImpl::refresh(const histogram_t* new_histogram_ptr) {
  std::fill(computed_quantiles_.begin(), computed_quantiles_.end(), 0.0);
  ASSERT(supportedQuantiles().size() == computed_quantiles_.size());
  hist_approx_quantile(new_histogram_ptr, supportedQuantiles().data(), supportedQuantiles().size(),
                       computed_quantiles_.data());
}

std::vector<CounterSharedPtr>& SourceImpl::cachedCounters() {
  if (!counters_) {
    counters_ = store_.counters();
  }
  return *counters_;
}
std::vector<GaugeSharedPtr>& SourceImpl::cachedGauges() {
  if (!gauges_) {
    gauges_ = store_.gauges();
  }
  return *gauges_;
}
std::vector<ParentHistogramSharedPtr>& SourceImpl::cachedHistograms() {
  if (!histograms_) {
    histograms_ = store_.histograms();
  }
  return *histograms_;
}

void SourceImpl::clearCache() {
  counters_.reset();
  gauges_.reset();
  histograms_.reset();
}

CounterSharedPtr RawStatDataAllocator::makeCounter(const std::string& name,
                                                   std::string&& tag_extracted_name,
                                                   std::vector<Tag>&& tags) {
  RawStatData* data = alloc(name);
  if (data == nullptr) {
    return nullptr;
  }
  return std::make_shared<CounterImpl>(*data, *this, std::move(tag_extracted_name),
                                       std::move(tags));
}

GaugeSharedPtr RawStatDataAllocator::makeGauge(const std::string& name,
                                               std::string&& tag_extracted_name,
                                               std::vector<Tag>&& tags) {
  RawStatData* data = alloc(name);
  if (data == nullptr) {
    return nullptr;
  }
  return std::make_shared<GaugeImpl>(*data, *this, std::move(tag_extracted_name), std::move(tags));
}

} // namespace Stats
} // namespace Envoy
