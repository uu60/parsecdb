// Every database operator checked against a plaintext evaluation of the same query: filters (each comparator, with
// and without compaction), both sorts, grouping with count, max, min and minAndMax, distinct, nested-loop, hash and
// left outer joins (also counted per group), and IN.
//
// Each check runs in four execution modes, switched in-process: every operator as one batch; batches of two rows (the
// tables have more, so the multi-batch paths run); batches of two rows without compaction; and baseline mode.
// Only rows marked valid are compared, so compaction may keep or drop invalid rows. Prints "FAIL" on a mismatch and
// exits non-zero on the client.

#include "parsecdb/db/DbConf.h"
#include "parsecdb/db/Table.h"
#include "parsecdb/db/View.h"
#include "parsecdb/db/Views.h"
#include "parsecdb/secret/Secrets.h"
#include "parsecdb/utils/System.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace parsecdb;

namespace {

using Row = std::vector<int64_t>;
using Rows = std::vector<Row>;

int g_tag = 0;
int nextTag() {
    g_tag += 1 << 16;
    return g_tag;
}

// Plaintext input, known to the client only. T(k, a, b) and R(k, v); keys 3 and 7 have no partner in R.
const Row T_K = {3, 1, 2, 3, 1, 5, 2, 2, 7, 3, 1, 9, 5};
const Row T_A = {10, 40, 25, 7, 3, 60, 25, 11, 2, 33, 40, 8, 14};
const Row T_B = {1, 2, 1, 1, 2, 2, 1, 3, 1, 2, 2, 1, 2};
const Row R_K = {1, 2, 2, 4, 5, 9, 11};
const Row R_V = {100, 200, 201, 400, 500, 900, 1100};

size_t tRows() { return T_K.size(); }
Row tRow(size_t i) { return {T_K[i], T_A[i], T_B[i]}; }

Row share(const Row& plain, int tid) {
    Row in = Comm::isClient() ? plain : Row{};
    return Secrets::boolShare(in, 2, 64, tid);
}

View makeView(const std::string& name, const std::vector<std::string>& fields, const std::vector<Row>& cols) {
    std::string tableName = name;
    std::vector<std::string> fieldNames = fields;
    std::vector<int> widths(fields.size(), 64);
    Table t(tableName, fieldNames, widths, "");
    for (size_t r = 0; r < cols[0].size(); ++r) {
        Row row;
        for (const auto& c : cols) {
            row.push_back(c[r]);
        }
        t.insert(row);
    }
    return Views::selectAll(t);
}

// Opens the named columns and the valid column of v to the client and returns the valid rows, in view order. The
// servers pass the view; on the client v is ignored.
Rows openValidRows(View& v, const std::vector<std::string>& names, int tid) {
    std::vector<Row> cols;
    for (size_t c = 0; c <= names.size(); ++c) {
        Row shares;
        if (Comm::isServer()) {
            const int idx = c < names.size() ? v.colIndex(names[c]) : v.colNum() + View::VALID_COL_OFFSET;
            if (idx < 0) {
                std::cout << "FAIL column " << names[c] << " not found" << std::endl;
            }
            shares = idx < 0 ? Row(v.rowNum(), 0) : v._dataCols[idx];
        }
        cols.push_back(Secrets::boolReconstruct(shares, 2, 64, tid));
    }
    Rows rows;
    if (Comm::isClient()) {
        for (size_t r = 0; r < cols[0].size(); ++r) {
            if (cols[names.size()][r] & 1) {
                Row row;
                for (size_t c = 0; c < names.size(); ++c) {
                    row.push_back(cols[c][r]);
                }
                rows.push_back(row);
            }
        }
    }
    return rows;
}

int failures = 0;
int checks = 0;

std::string str(const Rows& rows) {
    std::string s;
    for (const auto& r : rows) {
        s += " (";
        for (size_t i = 0; i < r.size(); ++i) {
            s += (i ? " " : "") + std::to_string(r[i]);
        }
        s += ")";
    }
    return s.empty() ? " (none)" : s;
}

// Compares as multisets unless ordered is set.
void expectRows(const std::string& label, Rows got, Rows expected, bool ordered = false) {
    if (!Comm::isClient()) {
        return;
    }
    ++checks;
    if (!ordered) {
        std::sort(got.begin(), got.end());
        std::sort(expected.begin(), expected.end());
    }
    if (got == expected) {
        std::cout << "ok " << label << std::endl;
        return;
    }
    ++failures;
    std::cout << "FAIL " << label << "\n  got:     " << str(got) << "\n  expected:" << str(expected) << std::endl;
}

Rows plainWhere(const std::function<bool(const Row&)>& keep) {
    Rows rows;
    for (size_t i = 0; i < tRows(); ++i) {
        if (keep(tRow(i))) {
            rows.push_back(tRow(i));
        }
    }
    return rows;
}

// Plaintext group aggregate over T, keyed by k.
Rows plainGroups(const std::function<int64_t(int64_t, int64_t)>& fold, const Row& col) {
    std::map<int64_t, int64_t> acc;
    for (size_t i = 0; i < tRows(); ++i) {
        auto it = acc.find(T_K[i]);
        acc[T_K[i]] = it == acc.end() ? col[i] : fold(it->second, col[i]);
    }
    Rows rows;
    for (auto [k, v] : acc) {
        rows.push_back({k, v});
    }
    return rows;
}

struct Mode {
    std::string name;
    int batchSize;
    bool noCompaction;
    bool baseline;
};

const std::vector<Mode> MODES = {
    {"one batch", 0, false, false},
    {"batches of 2", 2, false, false},
    {"batches of 2, no compaction", 2, true, false},
    {"baseline", 0, true, true},
};

void runChecks(const Mode& mode, int tid) {
    const std::string m = " [" + mode.name + "]";
    Row tagT(tRows()), tagR(R_K.size());
    for (size_t i = 0; i < tRows(); ++i) {
        tagT[i] = Views::hash(T_K[i]);
    }
    for (size_t i = 0; i < R_K.size(); ++i) {
        tagR[i] = Views::hash(R_K[i]);
    }
    auto ks = share(T_K, tid), as = share(T_A, tid), bs = share(T_B, tid), tts = share(tagT, tid);
    auto rks = share(R_K, tid), rvs = share(R_V, tid), rts = share(tagR, tid);
    View t, r, tTagged, rTagged;
    if (Comm::isServer()) {
        t = makeView("T", {"k", "a", "b"}, {ks, as, bs});
        r = makeView("R", {"k", "v"}, {rks, rvs});
        tTagged = makeView("T", {"k", "a", Table::BUCKET_TAG_PREFIX + "k"}, {ks, as, tts});
        rTagged = makeView("R", {"k", "v", Table::BUCKET_TAG_PREFIX + "k"}, {rks, rvs, rts});
    }
    const bool server = Comm::isServer();

    // Filters: each comparator against a = 25, then a conjunction, with and without compaction.
    const std::vector<std::tuple<View::ComparatorType, std::string, std::function<bool(int64_t)>>> comparators = {
        {View::LESS, "<", [](int64_t a) { return a < 25; }},
        {View::LESS_EQ, "<=", [](int64_t a) { return a <= 25; }},
        {View::GREATER, ">", [](int64_t a) { return a > 25; }},
        {View::GREATER_EQ, ">=", [](int64_t a) { return a >= 25; }},
        {View::EQUALS, "==", [](int64_t a) { return a == 25; }},
        {View::NOT_EQUALS, "!=", [](int64_t a) { return a != 25; }},
    };
    for (const auto& [op, opName, pred] : comparators) {
        View v = t;
        auto c = share({25}, tid);
        if (server) {
            std::vector<std::string> f = {"a"};
            std::vector<View::ComparatorType> ops = {op};
            v.filterAndConditions(f, ops, c, nextTag());
        }
        expectRows("filter a " + opName + " 25" + m, openValidRows(v, {"k", "a", "b"}, tid),
                   plainWhere([&](const Row& row) { return pred(row[1]); }));
    }
    for (bool clear : {true, false}) {
        View v = t;
        auto c = share({5, 3, 50}, tid);
        if (server) {
            std::vector<std::string> f = {"a", "b", "a"};
            std::vector<View::ComparatorType> ops = {View::GREATER, View::NOT_EQUALS, View::LESS_EQ};
            v.filterAndConditions(f, ops, c, clear, nextTag());
        }
        expectRows(std::string("filter a > 5 and b != 3 and a <= 50") + (clear ? "" : ", no clear") + m,
                   openValidRows(v, {"k", "a", "b"}, tid),
                   plainWhere([](const Row& row) { return row[1] > 5 && row[2] != 3 && row[1] <= 50; }));
    }

    // Sorts: the valid rows must come out in order.
    for (bool asc : {true, false}) {
        View v = t;
        if (server) {
            v.sort("a", asc, nextTag());
        }
        auto got = openValidRows(v, {"a"}, tid);
        Rows expected;
        for (auto a : T_A) {
            expected.push_back({a});
        }
        std::sort(expected.begin(), expected.end());
        if (!asc) {
            std::reverse(expected.begin(), expected.end());
        }
        expectRows(std::string("sort by a ") + (asc ? "asc" : "desc") + m, got, expected, true);
    }
    {
        View v = t;
        if (server) {
            std::vector<std::string> fields = {"k", "a"};
            std::vector<bool> asc = {true, false};
            v.sort(fields, asc, nextTag());
        }
        Rows expected = plainWhere([](const Row&) { return true; });
        std::sort(expected.begin(), expected.end(),
                  [](const Row& x, const Row& y) { return x[0] != y[0] ? x[0] < y[0] : x[1] > y[1]; });
        // Rows equal on (k, a) are equal on b too, so the full order is determined.
        expectRows("sort by k asc, a desc" + m, openValidRows(v, {"k", "a", "b"}, tid), expected, true);
    }

    // Grouped aggregates.
    {
        View v = t;
        if (server) {
            auto heads = v.groupBy("k", nextTag());
            v.max(heads, "a", "max_a", nextTag());
        }
        expectRows("group by k: max(a)" + m, openValidRows(v, {"k", "max_a"}, tid),
                   plainGroups([](int64_t x, int64_t y) { return std::max(x, y); }, T_A));
    }
    {
        View v = t;
        if (server) {
            auto heads = v.groupBy("k", nextTag());
            v.min(heads, "a", "min_a", nextTag());
        }
        expectRows("group by k: min(a)" + m, openValidRows(v, {"k", "min_a"}, tid),
                   plainGroups([](int64_t x, int64_t y) { return std::min(x, y); }, T_A));
    }
    {
        View v = t;
        if (server) {
            auto heads = v.groupBy("k", nextTag());
            v.minAndMax(heads, "a", "b", "min_a", "max_b", nextTag());
        }
        auto mins = plainGroups([](int64_t x, int64_t y) { return std::min(x, y); }, T_A);
        auto maxs = plainGroups([](int64_t x, int64_t y) { return std::max(x, y); }, T_B);
        Rows expected;
        for (size_t i = 0; i < mins.size(); ++i) {
            expected.push_back({mins[i][0], mins[i][1], maxs[i][1]});
        }
        expectRows("group by k: min(a), max(b)" + m, openValidRows(v, {"k", "min_a", "max_b"}, tid), expected);
    }
    {
        View v = t;
        if (server) {
            std::vector<std::string> groups = {"k"};
            auto heads = v.groupBy("k", nextTag());
            v.count(groups, heads, "cnt", nextTag());
        }
        expectRows("group by k: count" + m, openValidRows(v, {"k", "cnt"}, tid),
                   plainGroups([](int64_t x, int64_t) { return x + 1; }, Row(tRows(), 1)));
    }
    {
        View v = t;
        if (server) {
            std::vector<std::string> groups = {"k", "b"};
            auto heads = v.groupBy(groups, nextTag());
            v.count(groups, heads, "cnt", nextTag());
        }
        std::map<std::pair<int64_t, int64_t>, int64_t> counts;
        for (size_t i = 0; i < tRows(); ++i) {
            ++counts[{T_K[i], T_B[i]}];
        }
        Rows expected;
        for (auto [kb, c] : counts) {
            expected.push_back({kb.first, kb.second, c});
        }
        expectRows("group by k, b: count" + m, openValidRows(v, {"k", "b", "cnt"}, tid), expected);
    }
    {
        View v = t;
        if (server) {
            std::vector<std::string> fields = {"k", "b"};
            v.select(fields);
            v.distinct(nextTag());
        }
        std::set<Row> distinct;
        for (size_t i = 0; i < tRows(); ++i) {
            distinct.insert({T_K[i], T_B[i]});
        }
        expectRows("distinct k, b" + m, openValidRows(v, {"k", "b"}, tid), Rows(distinct.begin(), distinct.end()));
    }

    // Joins on k.
    Rows matches; // (T.k, T.a, T.b, R.v) for every matching pair
    for (size_t i = 0; i < tRows(); ++i) {
        for (size_t j = 0; j < R_K.size(); ++j) {
            if (T_K[i] == R_K[j]) {
                matches.push_back({T_K[i], T_A[i], T_B[i], R_V[j]});
            }
        }
    }
    {
        View j;
        if (server) {
            std::string f0 = "k", f1 = "k";
            View l = t, rr = r;
            j = Views::nestedLoopJoin(l, rr, f0, f1, true);
        }
        expectRows("nested-loop join T.k = R.k" + m, openValidRows(j, {"T.k", "T.a", "T.b", "R.v"}, tid), matches);
    }
    {
        View j;
        if (server) {
            std::string f0 = "k", f1 = "k";
            View l = tTagged, rr = rTagged;
            j = Views::hashJoin(l, rr, f0, f1, true);
        }
        Rows expected;
        for (const auto& row : matches) {
            expected.push_back({row[0], row[1], row[3]});
        }
        expectRows("hash join T.k = R.k" + m, openValidRows(j, {"T.k", "T.a", "R.v"}, tid), expected);
    }
    {
        View j;
        if (server) {
            std::string f0 = "k", f1 = "k";
            View l = t, rr = r;
            j = Views::leftOuterJoin(l, rr, f0, f1, false, true);
        }
        // Matched pairs, plus every T row without a partner, with zeros on the R side.
        Rows expected;
        for (const auto& row : matches) {
            expected.push_back({row[0], row[1], row[3], 1});
        }
        for (size_t i = 0; i < tRows(); ++i) {
            if (std::find(R_K.begin(), R_K.end(), T_K[i]) == R_K.end()) {
                expected.push_back({T_K[i], T_A[i], 0, 0});
            }
        }
        expectRows("left outer join T.k = R.k" + m, openValidRows(j, {"T.k", "T.a", "R.v", "$match:R"}, tid), expected);
    }
    {
        // Counted per R.k: unmatched T rows form the group R.k = 0 and count 0, as COUNT(R.k) in SQL.
        View j;
        if (server) {
            std::string f0 = "k", f1 = "k";
            View l = t, rr = r;
            j = Views::leftOuterJoin(l, rr, f0, f1, false, false);
            std::vector<std::string> groups = {"R.k"};
            auto heads = j.groupBy("R.k", nextTag());
            j.count(groups, heads, "cnt", false, nextTag());
        }
        std::map<int64_t, int64_t> counts = {{0, 0}};
        for (const auto& row : matches) {
            ++counts[row[0]];
        }
        Rows expected;
        for (auto [k, c] : counts) {
            expected.push_back({k, c});
        }
        expectRows("left outer join, group by R.k: count" + m, openValidRows(j, {"R.k", "cnt"}, tid), expected);
    }
    {
        View v = t;
        if (server) {
            View rr = r;
            v._dataCols[v.colNum() + View::VALID_COL_OFFSET] =
                Views::in(v._dataCols[0], rr._dataCols[0], v._dataCols[v.colNum() + View::VALID_COL_OFFSET],
                          rr._dataCols[rr.colNum() + View::VALID_COL_OFFSET]);
        }
        expectRows("T.k in R.k" + m, openValidRows(v, {"k", "a", "b"}, tid),
                   plainWhere([](const Row& row) { return std::find(R_K.begin(), R_K.end(), row[0]) != R_K.end(); }));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    System::init(argc, argv);
    DbConf::init();
    const int tid = System::nextTask() << (32 - Conf::TASK_TAG_BITS);
    g_tag = tid;

    for (const auto& mode : MODES) {
        Conf::BATCH_SIZE = mode.batchSize;
        DbConf::NO_COMPACTION = mode.noCompaction;
        DbConf::BASELINE_MODE = mode.baseline;
        runChecks(mode, tid);
    }

    const bool failed = Comm::isClient() && (failures > 0 || checks == 0);
    if (Comm::isClient()) {
        std::cout << (failed ? "FAIL" : "PASS") << " db operators: " << checks - failures << "/" << checks << " checks"
                  << std::endl;
    }
    System::finalize();
    return failed ? 1 : 0;
}
