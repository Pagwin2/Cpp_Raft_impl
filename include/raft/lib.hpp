#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <raft/rng.hpp>
#include <random>
#include <set>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifndef RAFT_LIB_SYMBOL
#define RAFT_LIB_SYMBOL

// on page 4 there's a 1 page reference sheet https://raft.github.io/raft.pdf
// TODO: implement everything under Rules for servers
namespace raft {

using id_t = std::uint64_t;
using index_t = std::uint64_t;
using term_t = std::uint64_t;

// convenience
using namespace std::chrono_literals;

// the base class of whatever actions can be applied to the state machine
class base_action {
  term_t term;

public:
  term_t get_term();
  void set_term(term_t);
  // inverse of parse
  virtual std::string describe() const = 0;
  // parse a string to get the action desired back
  // base_action parse(std::string);
};

template <typename Action> class base_state_machine {
public:
  static_assert(std::is_base_of_v<base_action, Action>);
  // need to be able to parse actions as well
  static_assert(std::is_function_v<typeof(Action::parse)>,
                "need to be able to parse out an action");
  // this method should be overriden by the real implementation
  virtual void apply(Action action) {
    // noop for base implementation
  }

public:
  base_state_machine() {}
};
enum mode { follower, candidate, leader };

enum io_action_variants {
  send_log,
  request_vote,
  domain_action,
  acknowledge_rpc
};
struct rpc_ack {
  io_action_variants ack_what;
  id_t my_id;
  id_t ack_receiver;
  bool successful;
};
class vote_request_state {
public:
  id_t candidate;
  id_t target;
  term_t candidate_term;
  index_t lastLogIndex;
  term_t lastLogTerm;

public:
  vote_request_state(id_t candidate, id_t target, term_t candidate_term,
                     index_t lastLogIndex, term_t lastLogTerm)
      : candidate(candidate), target(target), candidate_term(candidate_term),
        lastLogTerm(lastLogTerm), lastLogIndex(lastLogIndex) {}
  std::string line_separated() {
    // term_t term, id_t candidateId, index_t lastLogIndex, term_t lastLogTerm
    std::ostringstream ss;
    ss << candidate_term << std::endl;
    ss << candidate << std::endl;
    ss << lastLogIndex << std::endl;
    ss << lastLogTerm << std::endl;
    return ss.str();
  }
  id_t send_target() { return this->target; }
};

template <typename Action> class send_log_state {
public:
  std::vector<Action> actions;
  term_t leaderTerm;
  id_t leaderId;
  index_t prevLogIndex;
  term_t prevLogTerm;
  index_t committed;
  id_t target;
};
// Action is subclass of base_action which we can use for various things
// DomainAction is a misc action that isn't related to Raft that needs to be
// taken by whatever is doing io io_action is trying to be a tagged union
// telling the IO impl what to do
template <typename Action, typename DomainAction> class io_action {
  static_assert(std::is_base_of_v<base_action, Action>,
                "ActionType must inherit from base_action to ensure it has "
                "associated state");
  using content = std::variant<send_log_state<Action>, DomainAction, rpc_ack,
                               vote_request_state>;

  term_t sent_at;
  io_action_variants variant;
  content msg_contents;

public:
  io_action(io_action_variants variant, content msg, term_t term)
      : sent_at(term), msg_contents(msg), variant(variant) {}
  io_action_variants const &get_variant() { return this->variant; }
  id_t get_target() {
    switch (this->variant) {
    case io_action_variants::domain_action:
      throw "unreachable";
    case io_action_variants::send_log:
      return std::get<send_log_state<Action>>(this->msg_contents).target;
    case io_action_variants::acknowledge_rpc:
      return std::get<rpc_ack>(this->msg_contents).ack_receiver;
    case io_action_variants::request_vote:
      return std::get<vote_request_state>(this->msg_contents).send_target();
    default:
      throw "unreachable";
    }
  }
  term_t const &get_term() { return this->sent_at; }
  content const &contents() { return this->msg_contents; }
};
constexpr std::chrono::milliseconds timeout = 500ms;
template <typename Action, typename InnerMachine, typename DomainAction,
          typename rand_t = std::mt19937_64>
class state_machine {
protected:
  static_assert(std::is_base_of_v<base_action, Action>,
                "ActionType must inherit from base_action to ensure it has "
                "associated state");
  static_assert(std::is_base_of_v<base_state_machine<Action>, InnerMachine>,
                "The Inner State Machine needs to inherit from "
                "base_state_machine because C++ doesn't have interfaces");
  // static_assert(std::is_base_of_v<raft::base_rand, rand_t>,"The rng provider
  // doesn't implement base_rand");

  // normally I don't put _t after type names but id, index and term can easily
  // be var names so clarification seemed useful

  id_t myId;
  term_t currentTerm;
  // value used to indicate the id of the node we're following, either candidate
  // we voted for or current leader
  std::optional<id_t> following;
  std::set<id_t> siblings;
  std::map<id_t, index_t> replicatedIndices;
  std::vector<Action> log;
  index_t commitIndex;
  index_t lastApplied;
  mode currentState;
  std::deque<io_action<Action, DomainAction>> needed_actions;
  InnerMachine log_result;
  rand_t rand;

  // TODO: redo this in constructor
  // heartbeat and timer
  std::chrono::milliseconds electionTimeout =
      std::chrono::milliseconds(rand() % 151 + 150); // timer
  std::chrono::milliseconds time_since_heartbeat = 0ms;
  // std::chrono::milliseconds lastHeartbeat = std::chrono::steady_clock::now();
  // // beginning time
  int votes_recieved_counter = 0;

  void ack(io_action_variants act, bool success, id_t target) {
    this->needed_actions.push_back(
        io_action<Action, DomainAction>(io_action_variants::acknowledge_rpc,
                                        rpc_ack{.ack_what = act,
                                                .my_id = this->myId,
                                                .ack_receiver = target,
                                                .successful = success},
                                        this->currentTerm));
  }
  void send_log(send_log_state<Action> args) {
    this->needed_actions.push_back(io_action<Action, DomainAction>(
        io_action_variants::send_log, args, this->currentTerm));
  }
  void request_vote(vote_request_state request) {

    this->needed_actions.push_back(io_action<Action, DomainAction>(
        io_action_variants::request_vote, request, this->currentTerm));
  }

public:
  state_machine(std::set<id_t> siblings, id_t me, rand_t rand)
      : siblings(std::move(siblings)), myId(me), currentTerm(0), lastApplied(0),
        commitIndex(0), currentState(raft::mode::follower), log(),
        replicatedIndices(), following(std::nullopt), needed_actions(),
        rand(rand) {}
  // I really don't like that templated functions need to go in headers but oh
  // well might be sensible to trim the arg count down via a struct or class
  // which contains all this and builder pattern
  void append_entries(term_t term, id_t leaderId, index_t prevLogIndex,
                      term_t prevLogTerm, std::vector<Action> const &entries,
                      index_t leaderCommit) noexcept {
    if (term < this->currentTerm) {
      this->ack(io_action_variants::send_log, false, leaderId);
      return;
    }
    if (this->log.size() != 0 &&
        this->log[prevLogIndex].get_term() != prevLogTerm) {
      this->ack(io_action_variants::send_log, false, leaderId);
    }
    this->following.emplace(leaderId);
    this->currentTerm = term;
    std::optional<index_t> continue_idx = std::nullopt;
    for (index_t i = 0; i < entries.size(); i++) {
      if (i + prevLogIndex > this->log.size() - 1) {
        continue_idx = i;
        break;
      };
      if (this->log[i + prevLogIndex] != entries[i]) {
        this->log[i + prevLogIndex] = entries[i];
        this->log[i + prevLogIndex].set_term(this->currentTerm);
      }
    }
    if (continue_idx.has_value()) {
      for (index_t i = continue_idx.value(); i < entries.size(); i++) {
        this->log.push_back(entries[i]);
        this->log.back().set_term(this->currentTerm);
      }
    }
    this->commitIndex = std::max(this->commitIndex, leaderCommit);
    this->ack(io_action_variants::send_log, true, leaderId);
  }

  void request_votes(term_t term, id_t candidateId, index_t lastLogIndex,
                     term_t lastLogTerm) noexcept {
    if (term < this->currentTerm) {
      // return std::make_pair(this->currentTerm, false);
      this->ack(io_action_variants::request_vote, false, candidateId);
    }
    // if(term > this->currentTerm) std::remove_if(this->needed_actions.begin(),
    // this->needed_actions.end(), [](auto action){
    //     return false;
    // });

    if (!following.has_value() || following.value() == candidateId) {

      bool got_vote = this->log.size() == 0 ||
                      (lastLogTerm >=
                       (*std::max_element(this->log.begin(), this->log.end(),
                                          [](Action a, Action b) {
                                            return a.get_term() < b.get_term();
                                          }))
                           .get_term());

      if (got_vote) {
        this->following.emplace(candidateId);
        this->currentTerm = term;
      }
      // keep a list of things we want IO to do when it pings us with crank
      // again and apped this to that
      this->ack(io_action_variants::request_vote, got_vote, candidateId);
      // return std::make_pair(std::move(term), got_vote);
    } else {
      // keep a list of things we want IO to do when it pings us with crank
      // again and apped this to that
      this->ack(io_action_variants::request_vote, false, candidateId);
      // return std::make_pair(std::move(term), false);
    }
  }

  void ack_rpc(id_t ack_from, io_action_variants action, bool successful,
               term_t ack_from_term) {
    switch (this->currentState) {
    case mode::leader:
      // handle case where acknowledging a log appending as well as when being
      // demoted
      if (!successful) {
        // we got demoted, swapping to follower
        this->swap_follower();
        return;
      }
      switch (action) {
      case io_action_variants::request_vote:
        break;
      // we don't know anything about domain_actions so noop
      case io_action_variants::domain_action:
        break;
      case io_action_variants::acknowledge_rpc:
        // unreachable acknowledging an acknowledge is nonsense
        break;
      case io_action_variants::send_log:
        if (!successful) {
          if (ack_from_term > this->currentTerm) {
            this->currentTerm = ack_from_term;
            this->currentState = mode::follower;
            this->following = std::nullopt;
          } else {
            index_t i = 0;
            for (auto &action : this->log) {
              send_log_state<Action> s;
              s.leaderId = this->myId;
              s.target = ack_from;
              s.actions = {action};
              s.committed = this->commitIndex;
              s.leaderTerm = this->currentTerm;
              s.prevLogTerm = action.get_term();
              s.prevLogIndex = i;
              i++;
              this->send_log(s);
            }
          }
        }
        this->replicatedIndices[ack_from]++;
        std::set<index_t> indices;
        std::transform(this->replicatedIndices.begin(), replicatedIndices.end(),
                       std::inserter(indices, indices.begin()),
                       [](auto pair) { return pair.second; });
        this->commitIndex =
            std::max(this->commitIndex,
                     *std::min_element(indices.begin(), indices.end()));
        break;
      }
      break;
    case mode::candidate:
      switch (action) {
        {
        case io_action_variants::request_vote:
          if (successful) {
            this->votes_recieved_counter++;
            if (this->votes_recieved_counter > (this->siblings.size() / 2)) {
              this->currentState = mode::leader;
              this->following = std::nullopt;
              this->votes_recieved_counter = 0;
            }
          }
          break;
        default:
          break;
        }
      }
      // handle finding out not being up to date as well as getting another vote
      break;
    case mode::follower:
      // ignore any acks they can only be from when we were in a different state
      break;
    }
  }

  // change to follower state and remove io_actions which aren't ack
  void swap_follower() {
    auto follower_whitelist = [](io_action<Action, DomainAction> a) {
      return a.get_variant() != io_action_variants::acknowledge_rpc &&
             a.get_variant() != io_action_variants::domain_action;
    };
    this->currentState = mode::follower;
    // convoloted while loop to remove anything that doesn't match
    // follower_whitelist
    while (std::find_if(this->needed_actions.begin(),
                        this->needed_actions.end(),
                        follower_whitelist) != this->needed_actions.end()) {
      this->needed_actions.erase(std::find_if(this->needed_actions.begin(),
                                              this->needed_actions.end(),
                                              follower_whitelist));
    }
  }

  // InputIt is an iterator over values of type Action
  // Put actions into queue to be committed if we're the leader and return the
  // id of the leader if we aren't
  template <typename InputIt>
  std::optional<id_t> enqueue_actions(InputIt start, InputIt end) noexcept {
    static_assert(typeid(Action) == typeid(*start),
                  "Iterator must iterate over values of type Action (can't be "
                  "more specific due to this needing to be a string literal)");
    switch (this->currentState) {
    case mode::leader:
      std::move(start, end, std::back_inserter(this->log));
      return std::nullopt;
      break;
    case mode::candidate:
      // ???
      break;
    case mode::follower:
      return this->following;
      break;
    }
    return std::nullopt;
  }
  void prepend_heartbeat() {
    for (auto &sibling : this->siblings) {
      send_log_state<Action> s;
      s.target = sibling;
      s.actions = {};
      s.leaderId = this->myId;
      s.committed = this->commitIndex;
      s.leaderTerm = this->currentTerm;
      s.prevLogTerm = this->log.back().get_term();
      s.prevLogIndex = this->log.size() - 1;
      id_t tmp = sibling;
      this->needed_actions.push_front(io_action<Action, DomainAction>(
          io_action_variants::send_log, s, this->currentTerm));
    }
  }
  bool calling_election() {
    return this->time_since_heartbeat >= electionTimeout;
  }
  // method that tells the machine how long it's been since the last crank for
  // leadership elections and what not and allows it to process anything added
  // to the queue in the meantime return nullopt when no io needs to be done and
  // return the head of needed_actions if there's anything there
  std::optional<io_action<Action, DomainAction>>
  crank_machine(std::chrono::milliseconds time_passed) noexcept {

    this->time_since_heartbeat += time_passed;

    // if we have any log entries that are committed but not applied we should
    // apply them
    while (lastApplied < commitIndex) {
      this->log_result.apply(this->log[lastApplied + 1]);
      this->lastApplied++;
    }
    // check if we're due another election, if so become a candidate, increment
    // term and ask for votes (even if current state is leader)
    if (this->calling_election()) {
      this->currentState = mode::candidate;
      this->currentTerm++;
      this->following = this->myId;
      this->electionTimeout = std::chrono::milliseconds(rand() % 151 + 150);
      this->time_since_heartbeat = 0ms;
      this->votes_recieved_counter = 1;
      for (id_t s : this->siblings) {
        if (s != this->myId) {
          term_t log_term = 0;
          if (this->log.size() > 0) {
            log_term = this->log.back().get_term();
          }
          this->request_vote(vote_request_state(
              this->myId, s, this->currentTerm,
              this->log.size() != 0 ? this->log.size() - 1 : 0, log_term));
        }
      }
    }
    switch (this->currentState) {
    // if we're a follower we're done I think
    case mode::follower:
      break;
    // candidates should check if they should restart the election and if so
    // increment term and ask for votes
    case mode::candidate:
      // no additional logic needed just spooling out votes
      break;
    case mode::leader:
      if (this->commitIndex < this->log.size()) {
        std::vector<Action> send = {log[commitIndex]};
        for (auto sibling : siblings) {
          send_log_state<Action> s;
          s.leaderId = this->myId;
          s.target = sibling;
          s.actions = send;
          s.committed = this->commitIndex;
          s.leaderTerm = this->currentTerm;
          s.prevLogTerm = s.actions[0].get_term();
          s.prevLogIndex = commitIndex;
          this->send_log(s);
        }
        // send out a message to all the siblings about the uncommitted logs
      } else if (this->time_since_heartbeat > timeout) {
        this->prepend_heartbeat();
        this->time_since_heartbeat = 0ms;
      }

      break;
    }

    // pull an action from the queue
    if (this->needed_actions.size() == 0)
      return std::nullopt;
    else {
      io_action<Action, DomainAction> ret = this->needed_actions.front();
      this->needed_actions.pop_front();
      return ret;
    }
  }
};
id_t parse_id(std::string const &);
std::optional<io_action_variants> parse_action_variant(std::string const &);
term_t parse_term(std::string const &);
index_t parse_index(std::string const &);
std::string display_id(id_t const &);
std::string display_term(id_t const &);
std::string display_index(id_t const &);
std::string display_action_variant(io_action_variants const &);
template <typename Action>
std::vector<Action> parse_actions(std::string const &line) {
  static_assert(std::is_base_of_v<base_action, Action>);
  // need to be able to parse actions as well
  static_assert(std::is_function_v<typeof(Action::parse)>,
                "need to be able to parse out an action");

  std::istringstream stream(line);
  std::vector<Action> acts;
  do {
    std::string elem;
    std::getline(stream, elem, ',');
    if (elem == "")
      continue;
    acts.push_back(Action::parse(elem));
  } while (!stream.eof());
  return acts;
}
template <typename Action>
std::string display_actions(std::vector<Action> const &acts) {
  static_assert(std::is_base_of_v<base_action, Action>);
  // need to be able to parse actions as well
  static_assert(std::is_function_v<typeof(Action::parse)>,
                "need to be able to parse out an action");

  std::ostringstream stream;
  for (auto &act : acts) {
    stream << act.describe() << ",";
  }
  return stream.str();
}
} // namespace raft
#endif
