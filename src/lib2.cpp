#include <raft2/lib.hpp>
namespace raft {

template <> std::optional<u8> deserialize<u8>(u8 **buf, std::size_t &buf_size) {
  if (buf_size < 1) {
    return std::nullopt;
  }
  u8 ret = *buf[0];
  *buf += 1;
  buf_size -= 1;
  return ret;
}
template <>
std::optional<u64> deserialize<u64>(u8 **buff, std::size_t &buf_size) {
  if (buf_size < 8) {
    return std::nullopt;
  }
  u8 *buf = *buff;
  buf_size -= 8;
  u64 ret = (u64)buf[0] + ((u64)buf[1] << 8) + ((u64)buf[2] << 16) +
            ((u64)buf[3] << 24) + ((u64)buf[4] << 32) + ((u64)buf[5] << 40) +
            ((u64)buf[6] << 48) + ((u64)buf[7] << 56);
  *buff += 8;
  return ret;
}

template <>
std::optional<request_vote> deserialize<request_vote>(u8 **buff,
                                                      std::size_t &buf_size) {
  std::optional<msg_type> type = deserialize<msg_type>(buff, buf_size);
  if (!type.has_value() || type.value() != REQUEST_VOTE) {
    return std::nullopt;
  }

  id_t candidate = deserialize<id_t>(buff, buf_size).value();
  id_t request_receiver = deserialize<id_t>(buff, buf_size).value();
  term_t candidate_term = deserialize<term_t>(buff, buf_size).value();
  index_t last_log_index = deserialize<index_t>(buff, buf_size).value();
  term_t last_log_term = deserialize<term_t>(buff, buf_size).value();

  return request_vote{.candidate = candidate,
                      .request_receiver = request_receiver,
                      .candidate_term = candidate_term,
                      .last_log_index = last_log_index,
                      .last_log_term = last_log_term};
}

template <>
std::optional<acknowledge> deserialize<acknowledge>(u8 **buf,
                                                    std::size_t &buf_size) {}

} // namespace raft
