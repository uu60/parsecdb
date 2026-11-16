// Grouped aggregates (max, min, minAndMax, count, and count after a left outer join) checked against plaintext results,
// once with every operator as a single batch and once with batches of two rows run in parallel. The views have more
// rows than one batch, so the second run takes the multi-batch paths.
//
// Prints "FAIL" on a mismatch and exits non-zero on the client.

#include "parsecdb/db/DbConf.h"
#include "parsecdb/db/Table.h"
#include "parsecdb/db/View.h"
#include "parsecdb/db/Views.h"
#include "parsecdb/secret/Secrets.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/System.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace parsecdb;

namespace {

int g_tag = 0;
int nextTag() {
    g_tag += 1 << 16;
    return g_tag;
}

// Plaintext input, known to the client only.
const std::vector<int64_t> T_K = {3, 1, 2, 3, 1, 5, 2, 2, 7, 3, 1, 9, 5};
const std::vector<int64_t> T_A = {10, 40, 25, 7, 3, 60, 25, 11, 2, 33, 40, 8, 14};
const std::vector<int64_t> T_B = {1, 2, 1, 1, 2, 2, 1, 3, 1, 2, 2, 1, 2};
const std::vector<int64_t> R_K = {1, 2, 2, 4, 5, 9, 11};
const std::vector<int64_t> R_V = {100, 200, 201, 400, 500, 900, 1100};

std::vector<int64_t> share(const std::vector<int64_t>& plain, int tid) {
    std::vector<int64_t> in = Comm::isClient() ? plain : std::vector<int64_t>{};
    return Secrets::boolShare(in, 2, 64, tid);
}

View makeView(const std::string& name, const std::vector<std::string>& fields,
              const std::vector<std::vector<int64_t>>& cols) {
    std::string tableName = name;
    std::vector<std::string> fieldNames = fields;
    std::vector<int> widths(fields.size(), 64);
    Table t(tableName, fieldNames, widths, "");
    for (size_t r = 0; r < cols[0].size(); ++r) {
        std::vector<int64_t> row;
        for (const auto& c : cols) {
            row.push_back(c[r]);
        }
        t.insert(row);
    }
    return Views::selectAll(t);
}

// Opens the named columns and the valid column of v to the client, which keeps the rows marked valid. The servers
// pass the view; the client passes an empty one and the number of value columns.
std::vector<std::vector<int64_t>> openValidRows(View* v, const std::vector<std::string>& names, size_t valueCount,
                                                int tid) {
    std::vector<std::vector<int64_t>> cols;
    for (size_t c = 0; c <= valueCount; ++c) {
        std::vector<int64_t> shares;
        if (Comm::isServer()) {
            const int idx = c < valueCount ? v->colIndex(names[c]) : v->colNum() + View::VALID_COL_OFFSET;
            shares = v->_dataCols[idx];
        }
        cols.push_back(Secrets::boolReconstruct(shares, 2, 64, tid));
    }
    std::vector<std::vector<int64_t>> rows;
    if (Comm::isClient()) {
        for (size_t r = 0; r < cols[0].size(); ++r) {
            if (cols[valueCount][r] & 1) {
                std::vector<int64_t> row;
                for (size_t c = 0; c < valueCount; ++c) {
                    row.push_back(cols[c][r]);
                }
                rows.push_back(row);
            }
        }
        std::sort(rows.begin(), rows.end());
    }
    return rows;
}

int failures = 0;

void expectRows(const std::string& label, std::vector<std::vector<int64_t>> got,
                std::vector<std::vector<int64_t>> expected) {
    if (!Comm::isClient()) {
        return;
    }
    std::sort(expected.begin(), expected.end());
    if (got == expected) {
        std::cout << "ok " << label << std::endl;
        return;
    }
    ++failures;
    std::cout << "FAIL " << label << ": got";
    for (const auto& r : got) {
        std::cout << " (";
        for (auto x : r) {
            std::cout << x << ' ';
        }
        std::cout << ')';
    }
    std::cout << ", expected";
    for (const auto& r : expected) {
        std::cout << " (";
        for (auto x : r) {
            std::cout << x << ' ';
        }
        std::cout << ')';
    }
    std::cout << std::endl;
}

// Plaintext group aggregates over T, keyed by k.
std::vector<std::vector<int64_t>> plainGroups(int64_t (*fold)(int64_t, int64_t), const std::vector<int64_t>& col) {
    std::map<int64_t, int64_t> acc;
    for (size_t i = 0; i < T_K.size(); ++i) {
        auto it = acc.find(T_K[i]);
        acc[T_K[i]] = it == acc.end() ? col[i] : fold(it->second, col[i]);
    }
    std::vector<std::vector<int64_t>> rows;
    for (auto [k, v] : acc) {
        rows.push_back({k, v});
    }
    return rows;
}

int64_t maxOf(int64_t a, int64_t b) { return std::max(a, b); }
int64_t minOf(int64_t a, int64_t b) { return std::min(a, b); }

void runChecks(const std::string& mode, int tid) {
    auto ks = share(T_K, tid), as = share(T_A, tid), bs = share(T_B, tid);
    auto rks = share(R_K, tid), rvs = share(R_V, tid);
    View t, r;
    if (Comm::isServer()) {
        t = makeView("T", {"k", "a", "b"}, {ks, as, bs});
        r = makeView("R", {"k", "v"}, {rks, rvs});
    }

    {
        View v = t;
        if (Comm::isServer()) {
            auto heads = v.groupBy("k", nextTag());
            v.max(heads, "a", "max_a", nextTag());
        }
        expectRows(mode + " max", openValidRows(&v, {"k", "max_a"}, 2, tid), plainGroups(maxOf, T_A));
    }
    {
        View v = t;
        if (Comm::isServer()) {
            auto heads = v.groupBy("k", nextTag());
            v.min(heads, "a", "min_a", nextTag());
        }
        expectRows(mode + " min", openValidRows(&v, {"k", "min_a"}, 2, tid), plainGroups(minOf, T_A));
    }
    {
        View v = t;
        if (Comm::isServer()) {
            auto heads = v.groupBy("k", nextTag());
            v.minAndMax(heads, "a", "b", "min_a", "max_b", nextTag());
        }
        auto mins = plainGroups(minOf, T_A);
        auto maxs = plainGroups(maxOf, T_B);
        std::vector<std::vector<int64_t>> expected;
        for (size_t i = 0; i < mins.size(); ++i) {
            expected.push_back({mins[i][0], mins[i][1], maxs[i][1]});
        }
        expectRows(mode + " minAndMax", openValidRows(&v, {"k", "min_a", "max_b"}, 3, tid), expected);
    }
    {
        View v = t;
        if (Comm::isServer()) {
            std::vector<std::string> groups = {"k"};
            auto heads = v.groupBy("k", nextTag());
            v.count(groups, heads, "cnt", nextTag());
        }
        auto expected = plainGroups([](int64_t acc, int64_t) { return acc + 1; }, std::vector<int64_t>(T_K.size(), 1));
        expectRows(mode + " count", openValidRows(&v, {"k", "cnt"}, 2, tid), expected);
    }
    {
        // Left outer join T with R on k, counted per R.k: unmatched T rows form the group R.k = 0 and count 0.
        View j;
        if (Comm::isServer()) {
            std::string f0 = "k", f1 = "k";
            View l = t, rr = r;
            j = Views::leftOuterJoin(l, rr, f0, f1, false, false);
            std::vector<std::string> groups = {"R.k"};
            auto heads = j.groupBy("R.k", nextTag());
            j.count(groups, heads, "cnt", false, nextTag());
        }
        std::map<int64_t, int64_t> counts = {{0, 0}};
        for (auto tk : T_K) {
            for (auto rk : R_K) {
                if (tk == rk) {
                    ++counts[rk];
                }
            }
        }
        std::vector<std::vector<int64_t>> expected;
        for (auto [k, c] : counts) {
            expected.push_back({k, c});
        }
        expectRows(mode + " left outer join count", openValidRows(&j, {"R.k", "cnt"}, 2, tid), expected);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    System::init(argc, argv);
    DbConf::init();
    const int tid = System::nextTask() << (32 - Conf::TASK_TAG_BITS);
    g_tag = tid;

    // Every operator as one batch, then batches of two rows (each view has more rows than that).
    Conf::BATCH_SIZE = 0;
    runChecks("single-batch", tid);
    Conf::BATCH_SIZE = 2;
    runChecks("multi-batch", tid);

    const bool failed = Comm::isClient() && failures > 0;
    if (Comm::isClient()) {
        std::cout << (failed ? "FAIL" : "PASS") << " db aggregates" << std::endl;
    }
    System::finalize();
    return failed ? 1 : 0;
}
