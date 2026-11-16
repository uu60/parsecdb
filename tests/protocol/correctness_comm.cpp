// Comm layer correctness over the active backend (MPI, or TCP with --comm_type=tcp): blocking and asynchronous
// send/receive of int64 scalars, int64 vectors and strings between every pair of ranks, server-to-server exchange in
// both directions at once, empty and large payloads, and many interleaved tags. The whole suite runs twice, with
// transfer compression off and on (--comm_test_compression=false runs only the first pass).
//
// Every rank derives the payloads from the same seeded generator, so each receiver can check what it got. Values are
// ring elements of the given width (the contract of every caller in the library). With compression off they must
// arrive bit-exact; with compression on they only have to agree modulo 2^width, because narrow types are
// sign-extended on receive. Each rank logs its own mismatches; the client collects the servers' verdicts and prints
// the overall PASS/FAIL line.
#include "ProtocolTestSupport.h"

#include "parsecdb/conf/Conf.h"
#include "parsecdb/utils/System.h"

#include <array>
#include <thread>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
constexpr std::array<int, 7> WIDTHS{1, 2, 4, 8, 16, 32, 64};
constexpr int CLIENT = 2;

// The tag sequence is identical on every rank because every rank walks through the same cases in the same order.
// Tags stay below 2 * TAG_OFFSET, inside the 32767 that every MPI implementation must support. A second message of
// the same case uses tag + TAG_OFFSET.
constexpr int TAG_OFFSET = 16000;
int tagCounter = 1000;
int nextTag() {
    if (++tagCounter >= TAG_OFFSET) {
        tagCounter = 1000;
    }
    return tagCounter;
}

std::string phase() { return Conf::ENABLE_TRANSFER_COMPRESSION ? "compressed" : "plain"; }

std::vector<int64_t> payload(int salt, size_t n, int width) {
    auto rng = makeRng(salt);
    std::vector<int64_t> out(n);
    const auto edges = edgeValues(width);
    for (size_t i = 0; i < n; ++i) {
        out[i] = i < edges.size() ? edges[i] : randomRing(rng, width);
    }
    return out;
}

std::string stringPayload(int salt, size_t n) {
    auto rng = makeRng(salt);
    std::string out(n, '\0');
    for (size_t i = 0; i < n; ++i) {
        // Binary content: every byte value including NUL appears.
        out[i] = static_cast<char>(i < 256 ? i : rng() & 0xff);
    }
    return out;
}

bool same(int64_t expected, int64_t got, int width) {
    return Conf::ENABLE_TRANSFER_COMPRESSION ? ring(expected, width) == ring(got, width) : expected == got;
}

bool checkScalar(const std::string& name, int width, int64_t expected, int64_t got) {
    if (same(expected, got, width)) {
        return true;
    }
    Log::e("comm {} {} width={} MISMATCH expected={} got={}", phase(), name, width, expected, got);
    return false;
}

bool checkVector(const std::string& name, int width, const std::vector<int64_t>& expected,
                 const std::vector<int64_t>& got) {
    if (expected.size() != got.size()) {
        Log::e("comm {} {} width={} MISMATCH size expected={} got={}", phase(), name, width, expected.size(),
               got.size());
        return false;
    }
    int mismatches = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!same(expected[i], got[i], width) && ++mismatches <= 5) {
            Log::e("comm {} {} width={} MISMATCH index={} expected={} got={}", phase(), name, width, i, expected[i],
                   got[i]);
        }
    }
    if (mismatches > 0) {
        Log::e("comm {} {} width={} FAIL mismatches={}/{}", phase(), name, width, mismatches, expected.size());
    }
    return mismatches == 0;
}

bool checkString(const std::string& name, const std::string& expected, const std::string& got) {
    if (expected == got) {
        return true;
    }
    size_t at = 0;
    while (at < expected.size() && at < got.size() && expected[at] == got[at]) {
        ++at;
    }
    Log::e("comm {} {} MISMATCH size expected={} got={} first difference at {}", phase(), name, expected.size(),
           got.size(), at);
    return false;
}

// Ordered pairs (sender, receiver) over all three ranks.
const std::array<std::pair<int, int>, 6> PAIRS{{{2, 0}, {2, 1}, {0, 2}, {1, 2}, {0, 1}, {1, 0}}};

std::string pairName(const std::string& what, int from, int to) {
    return what + " " + std::to_string(from) + "->" + std::to_string(to);
}

// Blocking scalars and vectors at every width between every pair of ranks. Only the sender and the receiver of a
// pair take part, and they handle the messages in the same order, so blocking sends cannot deadlock.
bool testBlocking(size_t n, const std::string& label) {
    bool ok = true;
    const int me = Comm::rank();
    for (int width : WIDTHS) {
        for (auto [from, to] : PAIRS) {
            const int tag = nextTag();
            const auto values = payload(tag * 131 + width, n, width);
            const int64_t scalar = values.empty() ? ring(-1, width) : values.back();
            if (me == from) {
                Comm::send(scalar, width, to, tag);
                Comm::send(values, width, to, tag);
            } else if (me == to) {
                int64_t gotScalar = 0;
                std::vector<int64_t> got{42}; // must be replaced, not appended to
                Comm::receive(gotScalar, width, from, tag);
                Comm::receive(got, width, from, tag);
                ok = checkScalar(pairName("blocking scalar", from, to), width, scalar, gotScalar) && ok;
                ok = checkVector(pairName(label, from, to), width, values, got) && ok;
            }
        }
    }
    return ok;
}

bool testStrings() {
    bool ok = true;
    const int me = Comm::rank();
    for (size_t n : {size_t{0}, size_t{1}, size_t{5}, size_t{256}, size_t{1} << 20}) {
        for (auto [from, to] : PAIRS) {
            const int tag = nextTag();
            const auto text = stringPayload(tag, n);
            if (me == from) {
                Comm::send(text, to, tag);
            } else if (me == to) {
                std::string got = "stale";
                Comm::receive(got, from, tag);
                ok = checkString(pairName("string n=" + std::to_string(n), from, to), text, got) && ok;
            }
        }
    }
    return ok;
}

// Asynchronous sends and receives with known sizes, waited on after both sides have posted.
bool testAsync() {
    bool ok = true;
    const int me = Comm::rank();
    for (int width : WIDTHS) {
        for (auto [from, to] : PAIRS) {
            const int tag = nextTag();
            const int textTag = nextTag();
            const auto values = payload(tag * 17 + width, 333, width);
            const int64_t scalar = values[values.size() / 2];
            const auto text = stringPayload(tag, 300);
            if (me == from) {
                auto* r1 = Comm::sendAsync(scalar, width, to, tag);
                auto* r2 = Comm::sendAsync(values, width, to, tag + TAG_OFFSET);
                auto* r3 = Comm::sendAsync(text, to, textTag);
                Comm::wait(r1);
                Comm::wait(r2);
                Comm::wait(r3);
            } else if (me == to) {
                int64_t gotScalar = 0;
                std::vector<int64_t> got;
                std::string gotText;
                // Posted in the reverse order of the sends. Messages with the same sender and tag match in order, so
                // every message here has its own tag.
                auto* r3 = Comm::receiveAsync(gotText, static_cast<int>(text.size()), from, textTag);
                auto* r2 = Comm::receiveAsync(got, static_cast<int>(values.size()), width, from, tag + TAG_OFFSET);
                auto* r1 = Comm::receiveAsync(gotScalar, width, from, tag);
                Comm::wait(r3);
                Comm::wait(r2);
                Comm::wait(r1);
                ok = checkScalar(pairName("async scalar", from, to), width, scalar, gotScalar) && ok;
                ok = checkVector(pairName("async vector", from, to), width, values, got) && ok;
                ok = checkString(pairName("async string", from, to), text, gotText) && ok;
            }
        }
    }
    return ok;
}

// Both servers send to each other at the same time with serverSend/serverReceive (the send on a second thread), and
// with serverSendAsync/serverReceiveAsync. The client only takes the same tags, to keep its tag sequence in step.
bool testServerExchange() {
    bool ok = true;
    const int me = Comm::rank();
    const int other = 1 - me;
    for (int width : WIDTHS) {
        for (size_t n : {size_t{0}, size_t{1}, size_t{1000}, size_t{1} << 18}) {
            const int tag = nextTag();
            const int asyncTag = nextTag();
            const int textTag = nextTag();
            if (!Comm::isServer()) {
                continue;
            }
            const auto mine = payload(tag * 7 + me, n, width);
            const auto theirs = payload(tag * 7 + other, n, width);
            const std::string label = "server exchange n=" + std::to_string(n);

            std::thread sender([&]() {
                Comm::serverSend(mine, width, tag);
                if (!mine.empty()) {
                    Comm::serverSend(mine.front(), width, tag);
                }
            });
            std::vector<int64_t> got;
            Comm::serverReceive(got, width, tag);
            int64_t gotScalar = 0;
            if (!theirs.empty()) {
                Comm::serverReceive(gotScalar, width, tag);
            }
            sender.join();
            ok = checkVector(label, width, theirs, got) && ok;
            if (!theirs.empty()) {
                ok = checkScalar(label + " scalar", width, theirs.front(), gotScalar) && ok;
            }

            const std::string mineText = stringPayload(asyncTag * 3 + me, n % 4096);
            const std::string theirsText = stringPayload(asyncTag * 3 + other, n % 4096);
            std::vector<int64_t> gotAsync;
            std::string gotText;
            int64_t gotAsyncScalar = 0;
            const int64_t scalar = ring(asyncTag + me, width);
            auto* s1 = Comm::serverSendAsync(mine, width, asyncTag);
            auto* s2 = Comm::serverSendAsync(mineText, textTag);
            auto* s3 = Comm::serverSendAsync(scalar, width, asyncTag + TAG_OFFSET);
            auto* r1 = Comm::serverReceiveAsync(gotAsync, static_cast<int>(n), width, asyncTag);
            auto* r2 = Comm::serverReceiveAsync(gotText, static_cast<int>(theirsText.size()), textTag);
            auto* r3 = Comm::serverReceiveAsync(gotAsyncScalar, width, asyncTag + TAG_OFFSET);
            for (auto* r : {r1, r2, r3, s1, s2, s3}) {
                Comm::wait(r);
            }
            ok = checkVector("async " + label, width, theirs, gotAsync) && ok;
            ok = checkString("async " + label + " string", theirsText, gotText) && ok;
            ok = checkScalar("async " + label + " scalar", width, ring(asyncTag + other, width), gotAsyncScalar) && ok;
        }
    }
    return ok;
}

// Many messages with distinct tags from one sender to one receiver, received in a shuffled order. The sends are
// asynchronous, so a receiver that waits for the last tag first does not block the sender.
bool testInterleavedTags() {
    bool ok = true;
    const int me = Comm::rank();
    constexpr int messages = 64;
    for (auto [from, to] : PAIRS) {
        std::vector<int> tags(messages);
        for (auto& t : tags) {
            t = nextTag();
        }
        auto value = [](int tag) {
            return ring(static_cast<int64_t>(static_cast<uint64_t>(tag) * 0x9e3779b97f4a7c15ULL), 32);
        };
        if (me == from) {
            std::vector<int64_t> scalars(messages);
            std::vector<std::vector<int64_t>> vectors(messages);
            std::vector<AbstractRequest*> requests;
            for (int i = 0; i < messages; ++i) {
                scalars[i] = value(tags[i]);
                vectors[i] = payload(tags[i], static_cast<size_t>(i), 32);
                requests.push_back(Comm::sendAsync(scalars[i], 32, to, tags[i]));
                requests.push_back(Comm::sendAsync(vectors[i], 32, to, tags[i] + TAG_OFFSET));
            }
            for (auto* r : requests) {
                Comm::wait(r);
            }
        } else if (me == to) {
            std::vector<int> order(messages);
            for (int i = 0; i < messages; ++i) {
                order[i] = i;
            }
            auto rng = makeRng(from * 3 + to);
            std::shuffle(order.begin(), order.end(), rng);
            for (int i : order) {
                int64_t got = 0;
                std::vector<int64_t> gotVector;
                // Vector before scalar for odd messages: the two tags of one message are independent queues.
                if (i % 2 == 1) {
                    Comm::receive(gotVector, 32, from, tags[i] + TAG_OFFSET);
                    Comm::receive(got, 32, from, tags[i]);
                } else {
                    Comm::receive(got, 32, from, tags[i]);
                    Comm::receive(gotVector, 32, from, tags[i] + TAG_OFFSET);
                }
                const std::string name = pairName("tag " + std::to_string(tags[i]), from, to);
                ok = checkScalar(name, 32, value(tags[i]), got) && ok;
                ok = checkVector(name, 32, payload(tags[i], static_cast<size_t>(i), 32), gotVector) && ok;
            }
        }
    }
    return ok;
}

bool runSuite() {
    bool ok = true;
    ok = testBlocking(17, "blocking vector") && ok;
    ok = testBlocking(0, "empty vector") && ok;
    ok = testBlocking(size_t{1} << 20, "large vector") && ok;
    ok = testStrings() && ok;
    ok = testAsync() && ok;
    ok = testServerExchange() && ok;
    ok = testInterleavedTags() && ok;
    Log::i("comm {} suite on rank {}: {}", phase(), Comm::rank(), ok ? "ok" : "FAILED");
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);

    // The compression flag is read at every call, so it can be switched between the two passes. Every rank switches
    // at the same point, after all messages of the first pass have been received. --comm_test_compression=false skips
    // the compressed pass.
    const bool requested = Conf::ENABLE_TRANSFER_COMPRESSION;
    const bool compressedPass = AppConfig::getBool("comm_test_compression", true);
    bool passed = true;
    for (bool compression : {false, true}) {
        if (compression && !compressedPass) {
            Log::ir(CLIENT, "comm compressed suite skipped (--comm_test_compression=false)");
            continue;
        }
        Conf::ENABLE_TRANSFER_COMPRESSION = compression;
        passed = runSuite() && passed;
    }
    Conf::ENABLE_TRANSFER_COMPRESSION = requested;

    // Gather the servers' verdicts on the client.
    const int tag = nextTag();
    if (Comm::isServer()) {
        Comm::send(static_cast<int64_t>(passed ? 1 : 0), 64, CLIENT, tag);
    } else {
        for (int server : {0, 1}) {
            int64_t verdict = 0;
            Comm::receive(verdict, 64, server, tag);
            if (verdict != 1) {
                Log::e("comm: server {} reported mismatches", server);
                passed = false;
            }
        }
    }

    const int status = finish("Comm", passed);
    System::finalize();
    return status;
}
