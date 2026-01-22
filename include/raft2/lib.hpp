#include <cstdint>
#include <optional>
#include <raft2/helpers.hpp>
#include <utility>
#include <variant>
#include <vector>

namespace raft {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

using id_t = u64;
using index_t = u64;
using term_t = u64;
using msg_type = u8;

constexpr msg_type REQUEST_VOTE = 0;
struct request_vote {
  id_t candidate;
  id_t request_receiver;
  term_t candidate_term;
  index_t last_log_index;
  term_t last_log_term;
};

constexpr msg_type SEND_LOG = 1;

template <typename... Actions> struct send_log {
  term_t leader_term;
  id_t leader_id;
  index_t prev_log_index;
  term_t prev_log_term;
  index_t committed_up_to;
  id_t log_recipient;
  std::vector<std::variant<Actions...>> actions;
};

constexpr msg_type ACK = 2;
struct acknowledge {
  msg_type action_being_acked;
  id_t ack_sender;
  id_t ack_receiver;
  bool success;
};

constexpr msg_type EXTENSION = 255;

template <typename... Types>
using io_action =
    std::variant<request_vote, send_log<Types...>, acknowledge, Types...>;

template <typename... Types>

std::optional<std::size_t> serialize(io_action<> const &, u8 *, std::size_t);
template <typename Type, typename... Types>
// Trying to have a forwarding reference, we'll see how well that works
std::optional<std::size_t> serialize(io_action<Type, Types...> &&act, u8 *buf,
                                     std::size_t size) {

  return std::visit(
      overloaded{[buf, size](Type v) { return v.serialize(buf, size); },
                 [buf, size](io_action<Types...> &&v) {
                   return serialize(std::forward(v), buf, size);
                 }},
      std::forward(act));
}
template <typename T>
std::optional<T> deserialize(u8 **buf, std::size_t &buf_size);

template <typename... Ts>
concept MultiArg = requires(sizeof...(Ts) > 0);

template <typename... Actions>
std::optional<send_log<Actions...>> deserialize(u8 **buf, std::size_t &buf_size)

{
  std::optional<msg_type> type = deserialize<msg_type>(buf, buf_size);
  if (!type.has_value() || type.value() != SEND_LOG) {
    return std::nullopt;
  }
}

} // namespace raft
