#include "parsecdb/db/View.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

#include "Batching.h"
#include "parsecdb/compute/batch/arith/ArithMutexBatchOperator.h"
#include "parsecdb/db/DbConf.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/Math.h"
#include "parsecdb/utils/StringUtils.h"

// Every operator below has one implementation. planBatches() (see Batching.h) splits its rows into batches of
// Conf::BATCH_SIZE that run in parallel on the thread pool or, when multi-threading is disabled or BATCH_SIZE <= 0,
// into one batch that runs on the calling thread. Concurrent batches always use disjoint message tag ranges.
// DbConf::init() forces single-batch mode under BASELINE_MODE.

namespace parsecdb {

using namespace db;

View::View(std::vector<std::string>& fieldNames, std::vector<int>& fieldWidths)
    : Table(EMPTY_VIEW_NAME, fieldNames, fieldWidths, EMPTY_KEY_FIELD) {
    addRedundantCols();
}

View::View(std::string& tableName, std::vector<std::string>& fieldNames, std::vector<int>& fieldWidths)
    : Table(tableName, fieldNames, fieldWidths, EMPTY_KEY_FIELD) {
    addRedundantCols();
}

View::View(std::string& tableName, std::vector<std::string>& fieldNames, std::vector<int>& fieldWidths, bool dummy)
    : Table(tableName, fieldNames, fieldWidths, EMPTY_KEY_FIELD) {}

void View::select(std::vector<std::string>& fieldNames) {
    if (fieldNames.empty()) {
        return;
    }

    std::vector<int> selectedIndices;
    selectedIndices.reserve(fieldNames.size());

    for (const std::string& fieldName : fieldNames) {
        int index = colIndex(fieldName);
        if (index >= 0) {
            selectedIndices.push_back(index);
        }
    }

    if (selectedIndices.empty()) {
        return;
    }

    int totalCols = static_cast<int>(colNum());
    int validColIndex = totalCols + VALID_COL_OFFSET;
    int paddingColIndex = totalCols + PADDING_COL_OFFSET;

    std::vector<std::vector<int64_t>> newDataCols;
    std::vector<std::string> newFieldNames;
    std::vector<int> newFieldWidths;

    size_t newSize = selectedIndices.size() + 2;
    newDataCols.reserve(newSize);
    newFieldNames.reserve(newSize);
    newFieldWidths.reserve(newSize);

    for (int index : selectedIndices) {
        newDataCols.push_back(std::move(_dataCols[index]));
        newFieldNames.push_back(std::move(_fieldNames[index]));
        newFieldWidths.push_back(_fieldWidths[index]);
    }

    newDataCols.push_back(std::move(_dataCols[validColIndex]));
    newDataCols.push_back(std::move(_dataCols[paddingColIndex]));
    newFieldNames.push_back(VALID_COL_NAME);
    newFieldNames.push_back(PADDING_COL_NAME);
    newFieldWidths.push_back(1);
    newFieldWidths.push_back(1);

    _dataCols = std::move(newDataCols);
    _fieldNames = std::move(newFieldNames);
    _fieldWidths = std::move(newFieldWidths);
}

void View::sort(const std::string& orderField, bool ascendingOrder, int msgTagBase) {
    size_t n = rowNum();
    if (n == 0) {
        return;
    }
    bool isPowerOf2 = (n > 0) && ((n & (n - 1)) == 0);
    if (!isPowerOf2) {
        size_t nextPow2 = static_cast<size_t>(1) << static_cast<size_t>(std::ceil(std::log2(n)));
        for (auto& v : _dataCols) {
            v.resize(nextPow2, 1);
        }
    }
    bitonicSort(orderField, ascendingOrder, msgTagBase);
    if (n < _dataCols[0].size()) {
        for (auto& v : _dataCols) {
            v.resize(n);
        }
    }
}

void View::sort(const std::vector<std::string>& orderFields, const std::vector<bool>& ascendingOrders, int msgTagBase) {
    if (orderFields.empty()) {
        return;
    }

    if (orderFields.size() == 1) {
        sort(orderFields[0], ascendingOrders[0], msgTagBase);
        return;
    }

    size_t n = rowNum();
    if (n == 0) {
        return;
    }

    bool isPowerOf2 = (n > 0) && ((n & (n - 1)) == 0);
    if (!isPowerOf2) {
        size_t nextPow2 = static_cast<size_t>(1) << static_cast<size_t>(std::ceil(std::log2(n)));
        for (auto& v : _dataCols) {
            v.resize(nextPow2, 1);
        }
    }

    bitonicSort(orderFields, ascendingOrders, msgTagBase);

    if (n < _dataCols[0].size()) {
        for (auto& v : _dataCols) {
            v.resize(n);
        }
    }
}

int View::sortTagStride() {
    return ((rowNum() / 2 + Conf::BATCH_SIZE - 1) / Conf::BATCH_SIZE) * (colNum() - 1) *
           BoolMutexBatchOperator::tagStride();
}

int View::sortTagStride(const std::vector<std::string>& orderFields) {
    if (orderFields.empty()) {
        return 0;
    }

    if (orderFields.size() == 1) {
        return sortTagStride();
    }

    int base_stride = ((rowNum() / 2 + Conf::BATCH_SIZE - 1) / Conf::BATCH_SIZE) * (colNum() - 1) *
                      BoolMutexBatchOperator::tagStride();

    int multi_col_factor = static_cast<int>(orderFields.size() * 2);

    return base_stride * multi_col_factor;
}

void View::bitonicSort(const std::string& orderField, bool ascendingOrder, int msgTagBase) {
    if (rowNum() <= 1) {
        return;
    }
    const int ofi = colIndex(orderField);
    const int width = _fieldWidths[ofi];
    const int64_t rank = Comm::rank();
    // Each batch uses one tag range per swapped column; the comparison reuses the range of the first column.
    const int batchTagStride = BoolMutexBatchOperator::tagStride() * static_cast<int>(colNum() - 1);
    bitonicNetwork(
        batchTagStride, msgTagBase,
        [&](int cnt, BmtList& bmts) { bmts.addBitwise(BoolLessBatchOperator::bmtCount(cnt, width)); },
        [&](const std::vector<int>& xIdx, const std::vector<int>& yIdx, const std::vector<bool>& dirs, size_t begin,
            size_t end, int tag, BmtList& bmts) {
            const auto& orderCol = _dataCols[ofi];
            Shares xs, ys;
            xs.reserve(end - begin);
            ys.reserve(end - begin);
            for (size_t t = begin; t < end; ++t) {
                xs.push_back(orderCol[xIdx[t]]);
                ys.push_back(orderCol[yIdx[t]]);
            }
            auto bits = lessOp(xs, ys, width, tag, bmts.nextBitwise());
            for (size_t t = begin; t < end; ++t) {
                if (!(dirs[t] ^ !ascendingOrder)) {
                    bits[t - begin] ^= rank;
                }
            }
            return bits;
        });
}

void View::bitonicSort(const std::vector<std::string>& orderFields, const std::vector<bool>& ascendingOrders,
                       int msgTagBase) {
    if (rowNum() <= 1) {
        return;
    }
    std::vector<int> orderIdx(orderFields.size());
    for (size_t i = 0; i < orderFields.size(); i++) {
        orderIdx[i] = colIndex(orderFields[i]);
    }
    const int64_t rank = Comm::rank();
    const int maxOperatorTagStride = std::max({BoolLessBatchOperator::tagStride(), BoolEqualBatchOperator::tagStride(),
                                               BoolAndBatchOperator::tagStride(), BoolMutexBatchOperator::tagStride()});
    const int batchTagStride =
        maxOperatorTagStride + static_cast<int>(colNum() - 1) * BoolMutexBatchOperator::tagStride();
    bitonicNetwork(
        batchTagStride, msgTagBase,
        [&](int cnt, BmtList& bmts) {
            for (size_t f = 0; f < orderIdx.size(); f++) {
                const int width = _fieldWidths[orderIdx[f]];
                bmts.addBitwise(BoolLessBatchOperator::bmtCount(cnt, width));
                bmts.addBitwise(BoolEqualBatchOperator::bmtCount(cnt, width));
                if (f > 0) {
                    bmts.addBitwise(BoolMutexBatchOperator::bmtCount(cnt, 1));
                    bmts.addBitwise(BoolAndBatchOperator::bmtCount(cnt, 1));
                }
            }
        },
        [&](const std::vector<int>& xIdx, const std::vector<int>& yIdx, const std::vector<bool>& dirs, size_t begin,
            size_t end, int tag, BmtList& bmts) {
            // Lexicographic comparison: lts = "x before y" over the fields seen so far, eqs = all equal.
            Shares xs, ys, lts, eqs;
            for (size_t f = 0; f < orderIdx.size(); f++) {
                const auto& col = _dataCols[orderIdx[f]];
                const int width = _fieldWidths[orderIdx[f]];
                xs.clear();
                ys.clear();
                for (size_t t = begin; t < end; ++t) {
                    xs.push_back(col[xIdx[t]]);
                    ys.push_back(col[yIdx[t]]);
                }
                auto* lessBmts = bmts.nextBitwise();
                auto ltsF =
                    ascendingOrders[f] ? lessOp(xs, ys, width, tag, lessBmts) : lessOp(ys, xs, width, tag, lessBmts);
                auto eqsF = equalOp(xs, ys, width, tag, bmts.nextBitwise());
                if (f == 0) {
                    lts = std::move(ltsF);
                    eqs = std::move(eqsF);
                    continue;
                }
                lts = mutexOp(ltsF, lts, eqs, 1, tag, bmts.nextBitwise());
                eqs = andOp(eqs, eqsF, tag, bmts.nextBitwise());
            }
            for (size_t t = begin; t < end; ++t) {
                if (!dirs[t]) {
                    lts[t - begin] ^= rank;
                }
            }
            return lts;
        });
}

void View::bitonicNetwork(int batchTagStride, int msgTagBase, const PrepareSwapBitsFn& prepareSwapBits,
                          const SwapBitsFn& swapBits) {
    const int n = static_cast<int>(rowNum());
    auto& paddings = _dataCols[colNum() + PADDING_COL_OFFSET];
    std::vector<int> xIdx, yIdx;
    std::vector<bool> dirs;
    xIdx.reserve(n / 2);
    yIdx.reserve(n / 2);
    dirs.reserve(n / 2);
    for (int k = 2; k <= n; k <<= 1) {
        for (int j = k >> 1; j > 0; j >>= 1) {
            xIdx.clear();
            yIdx.clear();
            dirs.clear();
            for (int i = 0; i < n; ++i) {
                int l = i ^ j;
                if (l <= i) {
                    continue;
                }
                bool dir = (i & k) == 0;
                if (paddings[i] && paddings[l]) {
                    continue;
                }
                // Padding rows (public) move to the end without a secure comparison.
                if ((paddings[i] && dir) || (paddings[l] && !dir)) {
                    for (auto& v : _dataCols) {
                        std::swap(v[i], v[l]);
                    }
                    continue;
                }
                if (paddings[i] || paddings[l]) {
                    continue;
                }
                xIdx.push_back(i);
                yIdx.push_back(l);
                dirs.push_back(dir);
            }
            if (xIdx.empty()) {
                continue;
            }

            // Pairs of one step touch distinct rows, so their batches can run concurrently. Concurrent batches get
            // their BMTs polled here first: the comparison's, then one mutex per column (see swapRows).
            const BatchPlan plan = planBatches(xIdx.size());
            runBatches(
                plan,
                [&](int, size_t begin, size_t end) {
                    BmtList bmts(prefetchBmts(plan.parallel));
                    const int cnt = static_cast<int>(end - begin);
                    prepareSwapBits(cnt, bmts);
                    for (size_t c = 0; c + 1 < colNum(); ++c) {
                        bmts.addBitwise(BoolMutexBatchOperator::bmtCount(cnt, _fieldWidths[c]));
                    }
                    return bmts;
                },
                [&](int b, size_t begin, size_t end, BmtList& bmts) {
                    const int tag = msgTagBase + batchTagStride * b;
                    auto bits = swapBits(xIdx, yIdx, dirs, begin, end, tag, bmts);
                    swapRows(xIdx, yIdx, begin, end, bits, plan.parallel, tag, bmts);
                });
        }
    }
}

void View::swapRows(const std::vector<int>& xIdx, const std::vector<int>& yIdx, size_t begin, size_t end,
                    std::vector<int64_t>& swapBits, bool perColumn, int msgTagBase, BmtList& bmts) {
    const int cols = static_cast<int>(colNum()) - 1; // every column except the public padding column
    const size_t cnt = end - begin;

    if (perColumn) {
        // One concurrent mutex per column, each in its own tag range and with its BMTs taken here in column order.
        std::vector<std::vector<BitwiseBmt>*> colBmts(cols);
        for (int c = 0; c < cols; c++) {
            colBmts[c] = bmts.nextBitwise();
        }
        runTasks(cols, true, [&](int c) {
            auto& col = _dataCols[c];
            Shares xs(cnt), ys(cnt);
            for (size_t t = 0; t < cnt; ++t) {
                xs[t] = col[xIdx[begin + t]];
                ys[t] = col[yIdx[begin + t]];
            }
            auto zs = mutexBothOp(xs, ys, swapBits, _fieldWidths[c],
                                  msgTagBase + BoolMutexBatchOperator::tagStride() * c, colBmts[c]);
            for (size_t t = 0; t < cnt; ++t) {
                col[xIdx[begin + t]] = zs[t];
                col[yIdx[begin + t]] = zs[cnt + t];
            }
        });
        return;
    }

    // All columns in one mutex (one round); the swap bits repeat for every column. This runs on the calling thread
    // with nothing concurrent, so the operator obtains its own BMTs.
    Shares xs, ys;
    xs.reserve(cnt * cols);
    ys.reserve(cnt * cols);
    for (int c = 0; c < cols; c++) {
        const auto& col = _dataCols[c];
        for (size_t t = begin; t < end; ++t) {
            xs.push_back(col[xIdx[t]]);
            ys.push_back(col[yIdx[t]]);
        }
    }
    auto zs = mutexBothOp(xs, ys, swapBits, _maxWidth, msgTagBase);
    for (int c = 0; c < cols; c++) {
        auto& col = _dataCols[c];
        const int width = _fieldWidths[c];
        for (size_t t = 0; t < cnt; ++t) {
            col[xIdx[begin + t]] = Math::ring(zs[c * cnt + t], width);
            col[yIdx[begin + t]] = Math::ring(zs[(cols + c) * cnt + t], width);
        }
    }
}

namespace {

// Compares every element of data with the constant in consts (same size) and returns the condition bits.
Shares compareWithConst(Shares& data, Shares& consts, int width, View::ComparatorType ct, int msgTag,
                        std::vector<BitwiseBmt>* bmts, int taskTag) {
    Shares bits;
    bool negate = false;
    switch (ct) {
    case View::GREATER:
    case View::LESS_EQ:
        bits = lessOp(consts, data, width, msgTag, bmts, taskTag);
        negate = ct == View::LESS_EQ;
        break;
    case View::LESS:
    case View::GREATER_EQ:
        bits = lessOp(data, consts, width, msgTag, bmts, taskTag);
        negate = ct == View::GREATER_EQ;
        break;
    case View::EQUALS:
    case View::NOT_EQUALS:
        bits = equalOp(consts, data, width, msgTag, bmts, taskTag);
        negate = ct == View::NOT_EQUALS;
        break;
    }
    if (negate) {
        flipBits(bits);
    }
    return bits;
}

// ANDs all bit vectors with a balanced tree. The pairs of one level run concurrently, each in its own tag range.
Shares andAll(std::vector<Shares> level, int msgTagBase) {
    const bool parallel = !Conf::DISABLE_MULTI_THREAD;
    while (level.size() > 1) {
        const int pairs = static_cast<int>(level.size() / 2);
        auto next = runTasks(
            pairs, parallel,
            [&](int p) {
                BmtList bmts(prefetchBmts(parallel));
                bmts.addBitwise(BoolAndBatchOperator::bmtCount(static_cast<int>(level[2 * p].size()), 1));
                return bmts;
            },
            [&](int p, BmtList& bmts) {
                return andOp(level[2 * p], level[2 * p + 1], msgTagBase + p * BoolAndBatchOperator::tagStride(),
                             bmts.nextBitwise());
            });
        if (level.size() % 2) {
            next.push_back(std::move(level.back()));
        }
        level = std::move(next);
    }
    return std::move(level.front());
}

// Moves parts[from, from + count) out of parts and concatenates them.
Shares concatRange(std::vector<Shares>& parts, size_t from, size_t count) {
    std::vector<Shares> range;
    range.reserve(count);
    for (size_t i = from; i < from + count; ++i) {
        range.push_back(std::move(parts[i]));
    }
    return concat(std::move(range));
}

} // namespace

void View::filterAndConditions(std::vector<std::string>& fieldNames, std::vector<ComparatorType>& comparatorTypes,
                               std::vector<int64_t>& constShares, int msgTagBase) {
    filterAndConditions(fieldNames, comparatorTypes, constShares, true, msgTagBase);
}

void View::filterAndConditions(std::vector<std::string>& fieldNames, std::vector<ComparatorType>& comparatorTypes,
                               std::vector<int64_t>& constShares, bool clear, int msgTagBase) {
    const size_t condNum = fieldNames.size();
    std::vector<int> condCols(condNum, 0);
    for (size_t i = 0; i < condNum; i++) {
        for (size_t j = 0; j < _fieldNames.size(); j++) {
            if (_fieldNames[j] == fieldNames[i]) {
                condCols[i] = static_cast<int>(j);
                break;
            }
        }
    }

    // Every (condition, batch) pair is an independent task with its own tag range. Concurrent comparisons run in a
    // task tag namespace of their own, apart from the background BMT generators and from the surrounding query.
    const BatchPlan plan = planBatches(_dataCols[0].size());
    const int nb = plan.numBatches;
    const int tagOffset = std::max(BoolLessBatchOperator::tagStride(), BoolEqualBatchOperator::tagStride());
    // Concurrent comparisons run in their own task, apart from the query and the background BMT generators.
    const int taskTag = plan.parallel ? System::auxiliaryTask(1) : 0;
    auto isEquality = [&](int i) { return comparatorTypes[i] == EQUALS || comparatorTypes[i] == NOT_EQUALS; };
    auto parts = runTasks(
        static_cast<int>(condNum) * nb, plan.parallel,
        [&](int task) {
            const int i = task / nb;
            const int b = task % nb;
            const int cnt = static_cast<int>(plan.end(b) - plan.begin(b));
            const int width = _fieldWidths[condCols[i]];
            BmtList bmts(prefetchBmts(plan.parallel));
            bmts.addBitwise(isEquality(i) ? BoolEqualBatchOperator::bmtCount(cnt, width)
                                          : BoolLessBatchOperator::bmtCount(cnt, width));
            return bmts;
        },
        [&](int task, BmtList& bmts) {
            const int i = task / nb;
            const int b = task % nb;
            const int c = condCols[i];
            Shares buf;
            Shares* data = slice(_dataCols[c], plan.begin(b), plan.end(b), buf);
            Shares consts(data->size(), constShares[i]);
            return compareWithConst(*data, consts, _fieldWidths[c], comparatorTypes[i], msgTagBase + tagOffset * task,
                                    bmts.nextBitwise(), taskTag);
        });
    std::vector<Shares> conditions(condNum);
    for (size_t i = 0; i < condNum; i++) {
        conditions[i] = concatRange(parts, i * nb, nb);
    }
    auto result = andAll(std::move(conditions), msgTagBase);

    const int validColIndex = colNum() + VALID_COL_OFFSET;
    _dataCols[validColIndex] = andOp(result, _dataCols[validColIndex], msgTagBase);
    if (clear && !DbConf::BASELINE_MODE) {
        clearInvalidEntries(msgTagBase);
    }
}

void View::clearInvalidEntries(int msgTagBase) { clearInvalidEntries(true, msgTagBase); }

void View::clearInvalidEntries(bool doSort, int msgTagBase) {
    if (DbConf::BASELINE_MODE || DbConf::NO_COMPACTION) {
        return;
    }
    if (doSort) {
        sort(VALID_COL_NAME, false, msgTagBase);
    }

    // Number of valid rows, as an arithmetic share.
    int64_t sumShare = 0;
    auto& validCol = _dataCols[_dataCols.size() + VALID_COL_OFFSET];
    const BatchPlan plan = planBatches(validCol.size());
    auto partSums = runBatches(plan, [&](int b, size_t begin, size_t end) {
        Shares buf;
        auto arith =
            boolToArithOp(*slice(validCol, begin, end, buf), msgTagBase + BoolToArithBatchOperator::tagStride() * b);
        return std::accumulate(arith.begin(), arith.end(), static_cast<int64_t>(0));
    });
    for (auto s : partSums) {
        sumShare += s;
    }

    // Compaction reveals only the bit length of the number of valid rows and keeps the smallest power-of-two prefix
    // that can hold them, so the servers never learn the exact result size.
    {
        std::vector<int64_t> sv = {sumShare};
        auto sumBoolShare = arithToBoolOp(sv, msgTagBase)[0];
        std::vector<bool> bits(64);
        for (int i = 0; i < 64; i++) {
            bits[i] = Math::getBit(sumBoolShare, i);
        }
        int msb = -1;
        for (int b = 63; b > 0; --b) {
            int64_t bit_i = bits[b];
            int64_t bit_o = 0;

            auto s = Comm::serverSendAsync(bit_i, 1, msgTagBase);
            auto r = Comm::serverReceiveAsync(bit_o, 1, msgTagBase);
            Comm::wait(s);
            Comm::wait(r);

            int bit = static_cast<int>(bit_i ^ bit_o);
            if (bit) {
                msb = b;
                break;
            }
        }

        if (msb == 63) {
            return;
        }

        size_t n = rowNum();
        size_t keep = std::min(n, msb >= 0 ? static_cast<size_t>(1) << (msb + 1) : 2);

        for (auto& col : _dataCols) {
            col.resize(keep);
        }
    }
}

int View::clearInvalidEntriesTagStride() {
    return std::max(sortTagStride(), static_cast<int>((rowNum() + Conf::BATCH_SIZE - 1) / Conf::BATCH_SIZE) *
                                         BoolToArithBatchOperator::tagStride());
}

void View::addRedundantCols() {
    _fieldNames.emplace_back(View::VALID_COL_NAME);
    _fieldWidths.emplace_back(1);
    _dataCols.emplace_back(_dataCols[0].size(), Comm::rank());

    _fieldNames.emplace_back(View::PADDING_COL_NAME);
    _fieldWidths.emplace_back(1);
    _dataCols.emplace_back(_dataCols[0].size());
}

std::vector<int64_t> View::groupBy(const std::string& groupField, int msgTagBase) {
    return groupBy(groupField, true, msgTagBase);
}

std::vector<int64_t> View::groupBy(const std::string& groupField, bool doSort, int msgTagBase) {
    size_t n = rowNum();
    if (n == 0) {
        return std::vector<int64_t>();
    }

    if (doSort) {
        std::vector<std::string> sortFields = {VALID_COL_NAME, groupField};
        std::vector<bool> ascending = {false, true};
        sort(sortFields, ascending, msgTagBase);
        clearInvalidEntries(false, msgTagBase);
    }

    n = rowNum();
    if (n == 0) {
        return {};
    }
    const int k = colIndex(groupField);
    if (k < 0) {
        Log::e("groupBy: field '{}' not found", groupField);
        return std::vector<int64_t>(n, 0);
    }
    return groupHeads({k}, BoolEqualBatchOperator::tagStride(), msgTagBase);
}

std::vector<int64_t> View::groupBy(const std::vector<std::string>& groupFields, int msgTagBase) {
    size_t n = rowNum();
    if (n == 0 || groupFields.empty()) {
        return std::vector<int64_t>();
    }

    if (groupFields.size() == 1) {
        return groupBy(groupFields[0], msgTagBase);
    }

    {
        std::vector<std::string> sortFields = {VALID_COL_NAME};
        std::vector<bool> ascending = {false};
        for (const auto& gf : groupFields) {
            sortFields.push_back(gf);
            ascending.push_back(true);
        }
        sort(sortFields, ascending, msgTagBase);
        clearInvalidEntries(false, msgTagBase);
    }

    n = rowNum();
    if (n == 0) {
        return {};
    }
    std::vector<int> groupCols(groupFields.size());
    for (size_t k = 0; k < groupFields.size(); ++k) {
        groupCols[k] = colIndex(groupFields[k]);
        if (groupCols[k] < 0) {
            return std::vector<int64_t>(n, 0);
        }
    }
    const int tagOffset = std::max(BoolEqualBatchOperator::tagStride(), BoolAndBatchOperator::tagStride());
    return groupHeads(groupCols, tagOffset, msgTagBase);
}

std::vector<int64_t> View::groupHeads(const std::vector<int>& groupCols, int tagOffset, int msgTagBase) {
    const size_t n = rowNum();

    // eq[p] = (row p + 1 equals row p on every group column). Every (column, batch) pair is an independent task with
    // its own tag range.
    Shares eq;
    if (n > 1) {
        const BatchPlan plan = planBatches(n - 1);
        const int nb = plan.numBatches;
        auto parts = runTasks(
            static_cast<int>(groupCols.size()) * nb, plan.parallel,
            [&](int task) {
                const int b = task % nb;
                BmtList bmts(prefetchBmts(plan.parallel));
                bmts.addBitwise(BoolEqualBatchOperator::bmtCount(static_cast<int>(plan.end(b) - plan.begin(b)),
                                                                 _fieldWidths[groupCols[task / nb]]));
                return bmts;
            },
            [&](int task, BmtList& bmts) {
                const int c = groupCols[task / nb];
                const int b = task % nb;
                const auto& keys = _dataCols[c];
                const auto begin = static_cast<std::ptrdiff_t>(plan.begin(b));
                const auto end = static_cast<std::ptrdiff_t>(plan.end(b));
                Shares curr(keys.begin() + begin + 1, keys.begin() + end + 1);
                Shares prev(keys.begin() + begin, keys.begin() + end);
                return equalOp(curr, prev, _fieldWidths[c], msgTagBase + tagOffset * task, bmts.nextBitwise());
            });
        eq = concatRange(parts, 0, nb);
        for (size_t k = 1; k < groupCols.size(); ++k) {
            auto eqK = concatRange(parts, k * nb, nb);
            eq = andOp(eq, eqK, msgTagBase);
        }
    }

    std::vector<int64_t> heads(n, 0);
    // Compaction keeps invalid rows: a group starts at a valid row that differs from its predecessor.
    const auto& validCol = _dataCols[colNum() + VALID_COL_OFFSET];
    heads[0] = validCol[0];
    if (n > 1) {
        flipBits(eq);
        Shares validTail(validCol.begin() + 1, validCol.end());
        auto tail = andOp(validTail, eq, msgTagBase);
        std::copy(tail.begin(), tail.end(), heads.begin() + 1);
    }
    return heads;
}

int View::groupByTagStride() {
    size_t n = rowNum();
    if (n == 0) {
        return 0;
    }

    int tags_per_record = BoolEqualBatchOperator::tagStride() + BoolToArithBatchOperator::tagStride() +
                          ArithMutexBatchOperator::tagStride(64) + ArithMultiplyBatchOperator::tagStride(64);

    int sort_stride = sortTagStride();

    return sort_stride + static_cast<int>(n) * tags_per_record;
}

int View::distinctTagStride() {
    return std::max(
        {BoolEqualBatchOperator::tagStride(), BoolAndBatchOperator::tagStride(), clearInvalidEntriesTagStride()});
}

namespace {

// Rounds of a segmented (Hillis-Steele) scan over n rows. The round with distance delta processes the pairs
// (i, i + delta) for i in [0, n - delta): kernel(begin, end, delta, tag, bmts) handles the pairs [begin, end) of one
// batch and returns their updates, which apply(begin, delta, update) writes back once the whole round is done. Each
// batch of a round gets its own tag range of taskStride, and every round starts after the previous one. For
// concurrent batches, prepare(len, bmts) first polls on the calling thread the BMTs the kernel consumes for len
// pairs. Returns the first tag after all rounds.
template <typename Prepare, typename Kernel, typename Apply>
int segmentedScan(size_t n, int taskStride, int msgTagBase, Prepare&& prepare, Kernel&& kernel, Apply&& apply) {
    int tagCursor = msgTagBase;
    for (size_t delta = 1; delta < n; delta <<= 1) {
        const BatchPlan plan = planBatches(n - delta);
        auto updates = runBatches(
            plan,
            [&](int, size_t begin, size_t end) {
                BmtList bmts(prefetchBmts(plan.parallel));
                prepare(static_cast<int>(end - begin), bmts);
                return bmts;
            },
            [&](int b, size_t begin, size_t end, BmtList& bmts) {
                return kernel(begin, end, delta, tagCursor + b * taskStride, bmts);
            });
        tagCursor += plan.numBatches * taskStride;
        for (int b = 0; b < plan.numBatches; ++b) {
            apply(plan.begin(b), delta, updates[b]);
        }
    }
    return tagCursor;
}

} // namespace

std::vector<int64_t> View::groupTails(const std::vector<int64_t>& heads, int msgTagBase) {
    const size_t n = rowNum();
    const int64_t rank = Comm::rank();
    Shares tails(n, 0);
    for (size_t i = 0; i + 1 < n; ++i) {
        tails[i] = heads[i + 1];
    }
    // With invalid rows still present, the last valid row also ends its group: tails |= valid & !nextValid.
    auto& valid = _dataCols[colNum() + VALID_COL_OFFSET];
    Shares notNext(n, rank);
    for (size_t i = 0; i + 1 < n; ++i) {
        notNext[i] = valid[i + 1] ^ rank;
    }
    auto lastValid = andOp(valid, notNext, msgTagBase);
    flipBits(tails);
    flipBits(lastValid);
    tails = andOp(tails, lastValid, msgTagBase);
    flipBits(tails);
    return tails;
}

void View::applyGroupTails(int validIdx, std::vector<int64_t>& tails, bool compress, int msgTagBase) {
    if (DbConf::BASELINE_MODE) {
        _dataCols[validIdx] = andOp(_dataCols[validIdx], tails, msgTagBase);
        return;
    }
    _dataCols[validIdx] = andOp(_dataCols[validIdx], tails, msgTagBase);
    if (compress) {
        clearInvalidEntries(msgTagBase);
    }
}

void View::count(std::vector<std::string>& groupFields, std::vector<int64_t>& heads, std::string alias,
                 int msgTagBase) {
    count(groupFields, heads, std::move(alias), true, msgTagBase);
}

void View::count(std::vector<std::string>& groupFields, std::vector<int64_t>& heads, std::string alias, bool compress,
                 int msgTagBase) {
    size_t pos = groupFields[0].find('.');
    std::string matchedTable;
    if (groupFields.size() == 1 && pos != std::string::npos) {
        std::string fieldTable = groupFields[0].substr(0, pos);
        for (int i = 0; i < _fieldNames.size(); i++) {
            if (_fieldNames[i] == OUTER_MATCH_PREFIX + fieldTable) {
                matchedTable = fieldTable;
                break;
            }
        }
    }
    const int oldColNum = static_cast<int>(colNum());
    const int validIdx = oldColNum + VALID_COL_OFFSET;
    const int paddingIdx = oldColNum + PADDING_COL_OFFSET;

    std::vector<int> groupIdx;
    groupIdx.reserve(groupFields.size());
    for (const auto& name : groupFields) {
        int idx = colIndex(name);
        groupIdx.push_back(idx);
    }

    const size_t keep = groupIdx.size() + 2;
    std::vector<std::vector<int64_t>> newDataCols;
    std::vector<std::string> newFieldNames;
    std::vector<int> newFieldWidths;
    newDataCols.reserve(keep);
    newFieldNames.reserve(keep);
    newFieldWidths.reserve(keep);

    for (int gi : groupIdx) {
        newDataCols.push_back(std::move(_dataCols[gi]));
        newFieldNames.push_back(std::move(_fieldNames[gi]));
        newFieldWidths.push_back(_fieldWidths[gi]);
    }

    if (!matchedTable.empty()) {
        int mc = colIndex(View::OUTER_MATCH_PREFIX + matchedTable);
        newDataCols.push_back(std::move(_dataCols[mc]));
        newFieldNames.emplace_back(std::move(_fieldNames[mc]));
        newFieldWidths.push_back(1);
    }
    newDataCols.push_back(std::move(_dataCols[validIdx]));
    newDataCols.push_back(std::move(_dataCols[paddingIdx]));
    newFieldNames.emplace_back(VALID_COL_NAME);
    newFieldNames.emplace_back(PADDING_COL_NAME);
    newFieldWidths.push_back(1);
    newFieldWidths.push_back(1);

    _dataCols = std::move(newDataCols);
    _fieldNames = std::move(newFieldNames);
    _fieldWidths = std::move(newFieldWidths);

    countSorted(heads, alias, matchedTable, compress, msgTagBase);
}

void View::countSorted(std::vector<int64_t>& heads, const std::string& alias, const std::string& matchedTable,
                       bool compress, int msgTagBase) {
    const size_t n = rowNum();
    if (n == 0) {
        return;
    }

    const int validIdx = colNum() + VALID_COL_OFFSET;
    const int64_t rank = Comm::rank();

    // Compaction keeps invalid rows, so a segment starts at a valid group head, and only valid rows (and, after an
    // outer join, matched rows) count.
    Shares bsBool = andOp(heads, _dataCols[validIdx], msgTagBase);
    Shares counts = _dataCols[validIdx];
    // After an outer join, rows without a match on the counted side do not count (as COUNT(column) in SQL).
    if (!matchedTable.empty()) {
        counts = andOp(counts, _dataCols[colIndex(OUTER_MATCH_PREFIX + matchedTable)], msgTagBase);
    }

    auto countsArith = boolToArithOp(counts, msgTagBase);
    auto bsArith = boolToArithOp(bsBool, msgTagBase);

    // Segmented prefix sum: in each round, row i + delta adds the count of row i unless it starts a segment, and
    // becomes a segment start if either row is one.
    const int mulStride = ArithMultiplyBatchOperator::tagStride(64);
    const int andStride = BoolAndBatchOperator::tagStride();
    const int b2aStride = BoolToArithBatchOperator::tagStride();
    struct Update {
        Shares increments, bsBool, bsArith;
    };
    int tagCursor = segmentedScan(
        n, mulStride + andStride + b2aStride, msgTagBase,
        [&](int len, BmtList& bmts) {
            bmts.addArith(ArithMultiplyBatchOperator::bmtCount(len), 64);
            bmts.addBitwise(BoolAndBatchOperator::bmtCount(len, 1));
        },
        [&](size_t begin, size_t end, size_t delta, int tag, BmtList& bmts) {
            const size_t len = end - begin;
            Shares propagate(len), addValues(len), notLeft(len), notRight(len);
            for (size_t i = 0; i < len; ++i) {
                propagate[i] = rank - bsArith[begin + i + delta];
                addValues[i] = countsArith[begin + i];
                notLeft[i] = bsBool[begin + i] ^ rank;
                notRight[i] = bsBool[begin + i + delta] ^ rank;
            }
            Update u;
            u.increments = arithMultiplyOp(propagate, addValues, tag, bmts.nextArith());
            u.bsBool = andOp(notLeft, notRight, tag + mulStride, bmts.nextBitwise());
            flipBits(u.bsBool);
            u.bsArith = boolToArithOp(u.bsBool, tag + mulStride + andStride);
            return u;
        },
        [&](size_t begin, size_t delta, Update& u) {
            for (size_t i = 0; i < u.bsBool.size(); ++i) {
                countsArith[begin + i + delta] += u.increments[i];
                bsBool[begin + i + delta] = u.bsBool[i];
                bsArith[begin + i + delta] = u.bsArith[i];
            }
        });

    const int a2bStride = ArithToBoolBatchOperator::tagStride(64);
    const BatchPlan plan = planBatches(n);
    auto countsBool = concat(runBatches(
        plan,
        [&](int, size_t begin, size_t end) {
            BmtList bmts(prefetchBmts(plan.parallel));
            bmts.addBitwise(ArithToBoolBatchOperator::bmtCount(static_cast<int>(end - begin), 64));
            return bmts;
        },
        [&](int b, size_t begin, size_t end, BmtList& bmts) {
            Shares buf;
            return arithToBoolOp(*slice(countsArith, begin, end, buf), tagCursor + b * a2bStride, bmts.nextBitwise());
        }));

    auto tails = groupTails(heads, msgTagBase);

    const int insertPos = colNum() - 2;
    _fieldNames.insert(_fieldNames.begin() + insertPos, alias.empty() ? COUNT_COL_NAME : alias);
    _fieldWidths.insert(_fieldWidths.begin() + insertPos, 64);
    _dataCols.insert(_dataCols.begin() + insertPos, std::move(countsBool));

    applyGroupTails(colNum() + VALID_COL_OFFSET, tails, compress, msgTagBase);
}

void View::max(std::vector<int64_t>& heads, const std::string& fieldName, std::string alias, int msgTagBase) {
    if (rowNum() == 0) {
        return;
    }
    const int fieldIdx = colIndex(fieldName);
    if (fieldIdx < 0) {
        Log::e("Field '{}' not found for max operation", fieldName);
        return;
    }
    groupExtremes(heads, {{fieldIdx, _fieldWidths[fieldIdx], true, alias.empty() ? "max_" + fieldName : alias}},
                  msgTagBase);
}

void View::min(std::vector<int64_t>& heads, const std::string& fieldName, std::string alias, int msgTagBase) {
    if (rowNum() == 0) {
        return;
    }
    const int fieldIdx = colIndex(fieldName);
    if (fieldIdx < 0) {
        Log::e("Field '{}' not found for min operation", fieldName);
        return;
    }
    groupExtremes(heads, {{fieldIdx, _fieldWidths[fieldIdx], false, alias.empty() ? "min_" + fieldName : alias}},
                  msgTagBase);
}

void View::minAndMax(std::vector<int64_t>& heads, const std::string& fieldName, std::string minAlias,
                     std::string maxAlias, int msgTagBase) {
    minAndMax(heads, fieldName, fieldName, std::move(minAlias), std::move(maxAlias), msgTagBase);
}

void View::minAndMax(std::vector<int64_t>& heads, const std::string& minFieldName, const std::string& maxFieldName,
                     std::string minAlias, std::string maxAlias, int msgTagBase) {
    if (rowNum() == 0) {
        return;
    }
    const int minIdx = colIndex(minFieldName);
    const int maxIdx = colIndex(maxFieldName);
    if (minIdx < 0 || maxIdx < 0) {
        Log::e("Field not found for minAndMax: min='{}' idx={}, max='{}' idx={}", minFieldName, minIdx, maxFieldName,
               maxIdx);
        return;
    }
    groupExtremes(heads,
                  {{minIdx, _fieldWidths[minIdx], false, minAlias.empty() ? "min_" + minFieldName : minAlias},
                   {maxIdx, _fieldWidths[maxIdx], true, maxAlias.empty() ? "max_" + maxFieldName : maxAlias}},
                  msgTagBase);
}

void View::groupExtremes(std::vector<int64_t>& heads, const std::vector<ExtremeSpec>& specs, int msgTagBase) {
    const size_t n = rowNum();
    for (const auto& s : specs) {
        if (_dataCols[s.srcIdx].size() != n || heads.size() != n) {
            Log::e("Size mismatch: fieldData={}, heads={}, n={}", _dataCols[s.srcIdx].size(), heads.size(), n);
            return;
        }
    }

    const int64_t rank = Comm::rank();
    const int validIdx = colNum() + VALID_COL_OFFSET;
    const int k = static_cast<int>(specs.size());

    // Invalid rows take a value that never wins: 0 for max, the largest signed value for min.
    auto neutral = [&](int a, size_t len) {
        const int w = specs[a].width;
        return Shares(len, specs[a].isMax ? 0 : (1LL << (w - 1)) - 1);
    };

    std::vector<Shares> vs(k);
    for (int a = 0; a < k; ++a) {
        vs[a] = _dataCols[specs[a].srcIdx];
    }
    // Compaction keeps invalid rows: they neither start segments nor win.
    auto& valid = _dataCols[validIdx];
    Shares bsBool = andOp(heads, valid, msgTagBase);
    for (int a = 0; a < k; ++a) {
        auto fill = neutral(a, n);
        vs[a] = mutexOp(vs[a], fill, valid, specs[a].width, msgTagBase);
    }

    // Segmented scan: in each round, row i + delta takes the better of its value and row i's unless it starts a
    // segment. Per batch and aggregate a: comparison (and validity masking) at lessTag(a), candidate at candTag(a),
    // update at newTag(a); the segment flags last.
    const int lessStride = BoolLessBatchOperator::tagStride();
    const int mutexStride = BoolMutexBatchOperator::tagStride();
    const int andStride = BoolAndBatchOperator::tagStride();
    struct Update {
        std::vector<Shares> values;
        Shares bsBool;
    };
    segmentedScan(
        n, k * (lessStride + 2 * mutexStride) + andStride, msgTagBase,
        [&](int len, BmtList& bmts) {
            // In the kernel's order: validity masks, comparisons, candidates, updates, segment flags.
            for (int a = 0; a < k; ++a) {
                bmts.addBitwise(BoolMutexBatchOperator::bmtCount(len, specs[a].width));
                bmts.addBitwise(BoolMutexBatchOperator::bmtCount(len, specs[a].width));
            }
            for (int a = 0; a < k; ++a) {
                bmts.addBitwise(BoolLessBatchOperator::bmtCount(len, specs[a].width));
            }
            for (int round = 0; round < 2; ++round) {
                for (int a = 0; a < k; ++a) {
                    bmts.addBitwise(BoolMutexBatchOperator::bmtCount(len, specs[a].width));
                }
            }
            bmts.addBitwise(BoolAndBatchOperator::bmtCount(len, 1));
        },
        [&](size_t begin, size_t end, size_t delta, int tag, BmtList& bmts) {
            const size_t len = end - begin;
            auto lessTag = [&](int a) { return tag + a * lessStride; };
            auto candTag = [&](int a) { return tag + k * lessStride + a * mutexStride; };
            auto newTag = [&](int a) { return tag + k * (lessStride + mutexStride) + a * mutexStride; };

            std::vector<Shares> lefts(k), rights(k);
            for (int a = 0; a < k; ++a) {
                lefts[a].assign(vs[a].begin() + begin, vs[a].begin() + end);
                rights[a].assign(vs[a].begin() + begin + delta, vs[a].begin() + end + delta);
            }
            {
                Shares leftValid(valid.begin() + begin, valid.begin() + end);
                Shares rightValid(valid.begin() + begin + delta, valid.begin() + end + delta);
                for (int a = 0; a < k; ++a) {
                    auto fill = neutral(a, len);
                    lefts[a] = mutexOp(lefts[a], fill, leftValid, specs[a].width, lessTag(a), bmts.nextBitwise());
                    rights[a] = mutexOp(rights[a], fill, rightValid, specs[a].width, lessTag(a), bmts.nextBitwise());
                }
            }
            std::vector<Shares> lt(k), cand(k);
            for (int a = 0; a < k; ++a) {
                lt[a] = lessOp(lefts[a], rights[a], specs[a].width, lessTag(a), bmts.nextBitwise());
            }
            for (int a = 0; a < k; ++a) {
                auto* candBmts = bmts.nextBitwise();
                cand[a] = specs[a].isMax ? mutexOp(rights[a], lefts[a], lt[a], specs[a].width, candTag(a), candBmts)
                                         : mutexOp(lefts[a], rights[a], lt[a], specs[a].width, candTag(a), candBmts);
            }
            Shares notLeft(len), notRight(len);
            for (size_t i = 0; i < len; ++i) {
                notLeft[i] = bsBool[begin + i] ^ rank;
                notRight[i] = bsBool[begin + i + delta] ^ rank;
            }
            Update u;
            u.values.resize(k);
            for (int a = 0; a < k; ++a) {
                u.values[a] = mutexOp(cand[a], rights[a], notRight, specs[a].width, newTag(a), bmts.nextBitwise());
            }
            u.bsBool = andOp(notLeft, notRight, tag + k * (lessStride + 2 * mutexStride), bmts.nextBitwise());
            flipBits(u.bsBool);
            return u;
        },
        [&](size_t begin, size_t delta, Update& u) {
            for (int a = 0; a < k; ++a) {
                std::copy(u.values[a].begin(), u.values[a].end(), vs[a].begin() + begin + delta);
            }
            std::copy(u.bsBool.begin(), u.bsBool.end(), bsBool.begin() + begin + delta);
        });

    auto tails = groupTails(heads, msgTagBase);

    const int insertPos = colNum() - 2;
    for (int a = 0; a < k; ++a) {
        _fieldNames.insert(_fieldNames.begin() + insertPos + a, specs[a].outName);
        _fieldWidths.insert(_fieldWidths.begin() + insertPos + a, specs[a].width);
        _dataCols.insert(_dataCols.begin() + insertPos + a, std::move(vs[a]));
    }

    // The valid column moved behind the inserted aggregate columns.
    applyGroupTails(colNum() + VALID_COL_OFFSET, tails, true, msgTagBase);
}

void View::distinct(int msgTagBase) {
    size_t rn = rowNum();
    if (rn == 0) {
        return;
    }
    size_t cn = colNum();

    std::vector<std::string> fields;
    for (int i = 0; i < cn - 2; i++) {
        if (StringUtils::hasPrefix(_fieldNames[i], BUCKET_TAG_PREFIX)) {
            continue;
        }
        fields.push_back(_fieldNames[i]);
    }

    auto heads = groupBy(fields, msgTagBase);
    _dataCols[cn + VALID_COL_OFFSET] = std::move(heads);
    clearInvalidEntries(msgTagBase);
}

} // namespace parsecdb
