#pragma once

#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/common/pure.h"

#include "common/singleton/const_singleton.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ThriftProxy {

/**
 * Names of available Protocol implementations.
 */
class ProtocolNameValues {
public:
  // Binary protocol
  const std::string BINARY = "binary";

  // Lax Binary protocol
  const std::string LAX_BINARY = "binary/non-strict";

  // Compact protocol
  const std::string COMPACT = "compact";

  // JSON protocol
  const std::string JSON = "json";

  // Auto-detection protocol
  const std::string AUTO = "auto";
};

typedef ConstSingleton<ProtocolNameValues> ProtocolNames;

/**
 * Thrift protocol message types.
 * See https://github.com/apache/thrift/blob/master/lib/cpp/src/thrift/protocol/TProtocol.h
 */
enum class MessageType {
  Call = 1,
  Reply = 2,
  Exception = 3,
  Oneway = 4,

  // ATTENTION: MAKE SURE THIS REMAINS EQUAL TO THE LAST MESSAGE TYPE
  LastMessageType = Oneway,
};

/**
 * Thrift protocol struct field types.
 * See https://github.com/apache/thrift/blob/master/lib/cpp/src/thrift/protocol/TProtocol.h
 */
enum class FieldType {
  Stop = 0,
  Void = 1,
  Bool = 2,
  Byte = 3,
  Double = 4,
  I16 = 6,
  I32 = 8,
  I64 = 10,
  String = 11,
  Struct = 12,
  Map = 13,
  Set = 14,
  List = 15,

  // ATTENTION: MAKE SURE THIS REMAINS EQUAL TO THE LAST FIELD TYPE
  LastFieldType = List,
};

/**
 * ProtocolCallbacks are Thrift protocol-level callbacks.
 */
class ProtocolCallbacks {
public:
  virtual ~ProtocolCallbacks() {}

  /**
   * Indicates that the start of a Thrift protocol message was detected.
   * @param name the name of the message, if available
   * @param msg_type the type of the message
   * @param seq_id the message sequence id
   */
  virtual void messageStart(const absl::string_view name, MessageType msg_type,
                            int32_t seq_id) PURE;

  /**
   * Indicates that the start of a Thrift protocol struct was detected.
   * @param name the name of the struct, if available
   */
  virtual void structBegin(const absl::string_view name) PURE;

  /**
   * Indicates that the start of Thrift protocol struct field was detected.
   * @param name the name of the field, if available
   * @param field_type the type of the field
   * @param field_id the field id
   */
  virtual void structField(const absl::string_view name, FieldType field_type,
                           int16_t field_id) PURE;

  /**
   * Indicates that the end of a Thrift protocol struct was detected.
   */
  virtual void structEnd() PURE;

  /**
   * Indicates that the end of a Thrift protocol message was detected.
   */
  virtual void messageComplete() PURE;
};

/**
 * Protocol represents the operations necessary to implement the a generic Thrift protocol.
 * See https://github.com/apache/thrift/blob/master/doc/specs/thrift-protocol-spec.md
 */
class Protocol {
public:
  virtual ~Protocol() {}

  virtual const std::string& name() const PURE;

  /**
   * Reads the start of a Thrift protocol message from the buffer and updates the name, msg_type,
   * and seq_id parameters with values from the message header. If successful, the message header
   * is removed from the buffer.
   * @param buffer the buffer to read from
   * @param name updated with the message name on success only
   * @param msg_type updated with the MessageType on success only
   * @param seq_id updated with the message sequence ID on success only
   * @return true if a message header was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid message header
   */
  virtual bool readMessageBegin(Buffer::Instance& buffer, std::string& name, MessageType& msg_type,
                                int32_t& seq_id) PURE;

  /**
   * Reads the end of a Thrift protocol message from the buffer. If successful, the message footer
   * is removed from the buffer.
   * @param buffer the buffer to read from
   * @return true if a message footer was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid message footer
   */
  virtual bool readMessageEnd(Buffer::Instance& buffer) PURE;

  /**
   * Reads the start of a Thrift struct from the buffer and updates the name parameter with the
   * value from the struct header. If successful, the struct header is removed from the buffer.
   * @param buffer the buffer to read from
   * @param name updated with the struct name on success only
   * @return true if a struct header was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid struct header
   */
  virtual bool readStructBegin(Buffer::Instance& buffer, std::string& name) PURE;

  /**
   * Reads the end of a Thrift struct from the buffer. If successful, the struct footer is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @return true if a struct footer was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid struct footer
   */
  virtual bool readStructEnd(Buffer::Instance& buffer) PURE;

  /**
   * Reads the start of a Thrift struct field from the buffer and updates the name, field_type, and
   * field_id parameters with the values from the field header. If successful, the field header is
   * removed from the buffer.
   * @param buffer the buffer to read from
   * @param name updated with the field name on success only
   * @param field_type updated with the FieldType on success only
   * @param field_id updated with the field ID on success only
   * @return true if a field header was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid field header
   */
  virtual bool readFieldBegin(Buffer::Instance& buffer, std::string& name, FieldType& field_type,
                              int16_t& field_id) PURE;

  /**
   * Reads the end of a Thrift struct field from the buffer. If successful, the field footer is
   * removed from the buffer.
   * @param buffer the buffer to read from
   * @return true if a field footer was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid field footer
   */
  virtual bool readFieldEnd(Buffer::Instance& buffer) PURE;

  /**
   * Reads the start of a Thrift map from the buffer and updates the key_type, value_type, and size
   * parameters with the values from the map header. If successful, the map header is removed from
   * the buffer.
   * @param buffer the buffer to read from
   * @param key_type updated with map key FieldType on success only
   * @param value_type updated with map value FieldType on success only
   * @param size updated with the number of key-value pairs in the map on success only
   * @return true if a map header was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid map header
   */
  virtual bool readMapBegin(Buffer::Instance& buffer, FieldType& key_type, FieldType& value_type,
                            uint32_t& size) PURE;

  /**
   * Reads the end of a Thrift map from the buffer. If successful, the map footer is removed from
   * the buffer.
   * @param buffer the buffer to read from
   * @return true if a map footer was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid map footer
   */
  virtual bool readMapEnd(Buffer::Instance& buffer) PURE;

  /**
   * Reads the start of a Thrift list from the buffer and updates the elem_type, and size
   * parameters with the values from the list header. If successful, the list header is removed from
   * the buffer.
   * @param buffer the buffer to read from
   * @param elem_type updated with list element FieldType on success only
   * @param size updated with the number of list members on success only
   * @return true if a list header was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid list header
   */
  virtual bool readListBegin(Buffer::Instance& buffer, FieldType& elem_type, uint32_t& size) PURE;

  /**
   * Reads the end of a Thrift list from the buffer. If successful, the list footer is removed from
   * the buffer.
   * @param buffer the buffer to read from
   * @return true if a list footer was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid list footer
   */
  virtual bool readListEnd(Buffer::Instance& buffer) PURE;

  /**
   * Reads the start of a Thrift set from the buffer and updates the elem_type, and size
   * parameters with the values from the set header. If successful, the set header is removed from
   * the buffer.
   * @param buffer the buffer to read from
   * @param elem_type updated with set element FieldType on success only
   * @param size updated with the number of set members on success only
   * @return true if a set header was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set header
   */
  virtual bool readSetBegin(Buffer::Instance& buffer, FieldType& elem_type, uint32_t& size) PURE;

  /**
   * Reads the end of a Thrift set from the buffer. If successful, the set footer is removed from
   * the buffer.
   * @param buffer the buffer to read from
   * @return true if a set footer was sucessfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readSetEnd(Buffer::Instance& buffer) PURE;

  /**
   * Reads a boolean value from the buffer and updates value. If successful, the value is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readBool(Buffer::Instance& buffer, bool& value) PURE;

  /**
   * Reads a byte value from the buffer and updates value. If successful, the value is removed from
   * the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readByte(Buffer::Instance& buffer, uint8_t& value) PURE;

  /**
   * Reads a int16_t value from the buffer and updates value. If successful, the value is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readInt16(Buffer::Instance& buffer, int16_t& value) PURE;

  /**
   * Reads a int32_t value from the buffer and updates value. If successful, the value is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readInt32(Buffer::Instance& buffer, int32_t& value) PURE;

  /**
   * Reads a int64_t value from the buffer and updates value. If successful, the value is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readInt64(Buffer::Instance& buffer, int64_t& value) PURE;

  /**
   * Reads a double value from the buffer and updates value. If successful, the value is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readDouble(Buffer::Instance& buffer, double& value) PURE;

  /**
   * Reads a string value from the buffer and updates value. If successful, the value is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readString(Buffer::Instance& buffer, std::string& value) PURE;

  /**
   * Reads a binary value from the buffer and updates value. If successful, the value is removed
   * from the buffer.
   * @param buffer the buffer to read from
   * @param value updated with the value read from the buffer
   * @return true if a value successfully read, false if more data is required
   * @throw EnvoyException if the data is not a valid set footer
   */
  virtual bool readBinary(Buffer::Instance& buffer, std::string& value) PURE;

  /**
   * Writes the start of a Thrift protocol message to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param name the message name
   * @param msg_type the message's MessageType
   * @param seq_id the message sequende ID
   */
  virtual void writeMessageBegin(Buffer::Instance& buffer, const std::string& name,
                                 MessageType msg_type, int32_t seq_id) PURE;

  /**
   * Writes the end of a Thrift protocol message to the buffer.
   * @param buffer Buffer::Instance to modify
   */
  virtual void writeMessageEnd(Buffer::Instance& buffer) PURE;

  /**
   * Writes the start of a Thrift struct to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param name the struct name, if known
   */
  virtual void writeStructBegin(Buffer::Instance& buffer, const std::string& name) PURE;

  /**
   * Writes the end of a Thrift struct to the buffer.
   * @param buffer Buffer::Instance to modify
   */
  virtual void writeStructEnd(Buffer::Instance& buffer) PURE;

  /**
   * Writes the start of a Thrift struct field to the buffer
   * @param buffer Buffer::Instance to modify
   * @param name the field name, if known
   * @param field_type the field's FieldType
   * @param field_id the field ID
   */
  virtual void writeFieldBegin(Buffer::Instance& buffer, const std::string& name,
                               FieldType field_type, int16_t field_id) PURE;

  /**
   * Writes the end of a Thrift struct field to the buffer.
   * @param buffer Buffer::Instance to modify
   */
  virtual void writeFieldEnd(Buffer::Instance& buffer) PURE;

  /**
   * Writes the start of a Thrift map to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param key_type the map key FieldType
   * @param value_type the map value FieldType
   * @param size the number of key-value pairs in the map
   */
  virtual void writeMapBegin(Buffer::Instance& buffer, FieldType key_type, FieldType value_type,
                             uint32_t size) PURE;

  /**
   * Writes the end of a Thrift map to the buffer.
   * @param buffer Buffer::Instance to modify
   */
  virtual void writeMapEnd(Buffer::Instance& buffer) PURE;

  /**
   * Writes the start of a Thrift list to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param elem_type the list element FieldType
   * @param size the number of list members
   */
  virtual void writeListBegin(Buffer::Instance& buffer, FieldType elem_type, uint32_t size) PURE;

  /**
   * Writes the end of a Thrift list to the buffer.
   * @param buffer Buffer::Instance to modify
   */
  virtual void writeListEnd(Buffer::Instance& buffer) PURE;

  /**
   * Writes the start of a Thrift set to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param elem_type the set element FieldType
   * @param size the number of set members
   */
  virtual void writeSetBegin(Buffer::Instance& buffer, FieldType elem_type, uint32_t size) PURE;

  /**
   * Writes the end of a Thrift set to the buffer.
   * @param buffer Buffer::Instance to modify
   */
  virtual void writeSetEnd(Buffer::Instance& buffer) PURE;

  /**
   * Writes a boolean value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value bool to write
   */
  virtual void writeBool(Buffer::Instance& buffer, bool value) PURE;

  /**
   * Writes a byte value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value uint8_t to write
   */
  virtual void writeByte(Buffer::Instance& buffer, uint8_t value) PURE;

  /**
   * Writes a int16_t value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value int16_t to write
   */
  virtual void writeInt16(Buffer::Instance& buffer, int16_t value) PURE;

  /**
   * Writes a int32_t value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value int32_t to write
   */
  virtual void writeInt32(Buffer::Instance& buffer, int32_t value) PURE;

  /**
   * Writes a int64_t value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value int64_t to write
   */
  virtual void writeInt64(Buffer::Instance& buffer, int64_t value) PURE;

  /**
   * Writes a double value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value double to write
   */
  virtual void writeDouble(Buffer::Instance& buffer, double value) PURE;

  /**
   * Writes a string value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value std::string to write
   */
  virtual void writeString(Buffer::Instance& buffer, const std::string& value) PURE;

  /**
   * Writes a binary value to the buffer.
   * @param buffer Buffer::Instance to modify
   * @param value std::string to write
   */
  virtual void writeBinary(Buffer::Instance& buffer, const std::string& value) PURE;
};

typedef std::unique_ptr<Protocol> ProtocolPtr;

} // namespace ThriftProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
