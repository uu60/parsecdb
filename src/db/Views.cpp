#include "parsecdb/db/Views.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "Batching.h"
#include "parsecdb/comm/Comm.h"
#include "parsecdb/db/DbConf.h"
#include "parsecdb/system/System.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/Math.h"

// Batched work follows the scheme described in View.cpp: planBatches() decides between parallel batches of
// Conf::BATCH_SIZE and a single batch on the calling thread.

namespace parsecdb {

using namespace db;

namespace {

// Task tag of nestedLoopJoin()'s operators, whose message tags carry no task prefix: keeps them apart from the
// background BMT generators, which own the lowest task tags.
constexpr int JOIN_TASK_TAG = 4;

// Message tag base, with the foreground task prefix, for the operators below that have no msgTagBase parameter.
// A plain 0 would put their traffic in task 0, which belongs to a background BMT generator.
int foregroundTagBase() { return System::nextTask() << (32 - Conf::TASK_TAG_BITS); }

} // namespace

View Views::selectAll(Table& t) {
    View v(t._tableName, t._fieldNames, t._fieldWidths);
    v._dataCols = t._dataCols;
    v._dataCols.emplace_back(t._dataCols[0].size(), Comm::rank());
    v._dataCols.emplace_back(t._dataCols[0].size());
    return v;
}

View Views::selectAllWithFieldPrefix(Table& t) {
    std::vector<std::string> fieldNames(t._fieldNames.size());
    for (int i = 0; i < t._fieldNames.size(); i++) {
        fieldNames[i] = getAliasColName(t._tableName, t._fieldNames[i]);
    }

    View v(t._tableName, fieldNames, t._fieldWidths);
    v._dataCols = t._dataCols;
    v._dataCols.emplace_back(t.rowNum(), Comm::rank());
    v._dataCols.emplace_back(t.rowNum());
    return v;
}

View Views::selectColumns(Table& t, std::vector<std::string>& fieldNames) {
    std::vector<size_t> indices;
    indices.reserve(fieldNames.size());
    for (auto& name : fieldNames) {
        auto it = std::find(t._fieldNames.begin(), t._fieldNames.end(), name);
        indices.push_back(std::distance(t._fieldNames.begin(), it));
    }

    std::vector<int> widths;
    widths.reserve(indices.size());
    for (auto idx : indices) {
        widths.push_back(t._fieldWidths[idx]);
    }

    View v(t._tableName, fieldNames, widths);

    if (t._dataCols.empty()) {
        return v;
    }

    int colIndex = 0;
    for (auto idx : indices) {
        v._dataCols[colIndex++] = t._dataCols[idx];
    }

    v._dataCols[v.colNum() + View::PADDING_COL_OFFSET] = std::vector<int64_t>(v.rowNum(), 0);
    v._dataCols[v.colNum() + View::VALID_COL_OFFSET] = std::vector<int64_t>(v.rowNum(), Comm::rank());

    return v;
}

View Views::nestedLoopJoin(View& v0, View& v1, std::string& field0, std::string& field1, bool compress) {
    const size_t effectiveFieldNum0 = v0.colNum() - 2;
    const size_t effectiveFieldNum1 = v1.colNum() - 2;

    std::vector<std::string> fieldNames(effectiveFieldNum0 + effectiveFieldNum1);
    std::string tableName0 = v0._tableName.empty() ? "$t0" : v0._tableName;
    std::string tableName1 = v1._tableName.empty() ? "$t1" : v1._tableName;

    const std::string prefix = Table::BUCKET_TAG_PREFIX;
    auto decorate = [&](const std::string& tableName, const std::string& raw) -> std::string {
        if (raw.compare(0, prefix.size(), prefix) == 0) {
            return prefix + tableName + "." + raw.substr(prefix.size());
        } else {
            return tableName + "." + raw;
        }
    };

    for (size_t i = 0; i < effectiveFieldNum0; ++i)
        fieldNames[i] = decorate(tableName0, v0._fieldNames[i]);
    for (size_t i = 0; i < effectiveFieldNum1; ++i)
        fieldNames[i + effectiveFieldNum0] = decorate(tableName1, v1._fieldNames[i]);

    std::vector<int> fieldWidths(effectiveFieldNum0 + effectiveFieldNum1);
    for (size_t i = 0; i < effectiveFieldNum0; ++i)
        fieldWidths[i] = v0._fieldWidths[i];
    for (size_t i = 0; i < effectiveFieldNum1; ++i)
        fieldWidths[i + effectiveFieldNum0] = v1._fieldWidths[i];

    View joined(fieldNames, fieldWidths);
    if (v0._dataCols.empty() || v1._dataCols.empty())
        return joined;

    const size_t rows0 = v0.rowNum();
    const size_t rows1 = v1.rowNum();
    if (rows0 == 0 || rows1 == 0)
        return joined;

    const size_t n = rows0 * rows1;
    for (int i = 0; i < joined.colNum(); ++i) {
        joined._dataCols[i].resize(n);
    }

    const int colIndex0 = v0.colIndex(field0);
    const int colIndex1 = v1.colIndex(field1);
    if (colIndex0 == -1 || colIndex1 == -1)
        return joined;

    auto& col0 = v0._dataCols[colIndex0];
    auto& col1 = v1._dataCols[colIndex1];
    const int keyWidth = v0._fieldWidths[colIndex0];

    const bool BASELINE = DbConf::BASELINE_MODE;

    const int validIdx0 = v0.colNum() + View::VALID_COL_OFFSET;
    const int validIdx1 = v1.colNum() + View::VALID_COL_OFFSET;
    const auto& lvalid = v0._dataCols[validIdx0];
    const auto& rvalid = v1._dataCols[validIdx1];

    // Output row g joins row g / rows1 of v0 with row g % rows1 of v1.
    size_t rowIndex = 0;
    for (size_t i = 0; i < rows0; ++i) {
        for (size_t j = 0; j < rows1; ++j) {
            for (size_t k = 0; k < effectiveFieldNum0; ++k)
                joined._dataCols[k][rowIndex] = v0._dataCols[k][i];
            for (size_t k = 0; k < effectiveFieldNum1; ++k)
                joined._dataCols[k + effectiveFieldNum0][rowIndex] = v1._dataCols[k][j];
            ++rowIndex;
        }
    }

    // Validity of each output row: keys equal and both input rows valid.
    const int eqStride = BoolEqualBatchOperator::tagStride();
    const int andStride = BoolAndBatchOperator::tagStride();
    const int blockStride = eqStride + 2 * andStride;
    const BatchPlan plan = planBatches(n);
    auto prepare = [&](int, size_t begin, size_t end) {
        BmtList bmts(prefetchBmts(plan.parallel));
        const int cnt = static_cast<int>(end - begin);
        bmts.addBitwise(BoolEqualBatchOperator::bmtCount(cnt, keyWidth));
        bmts.addBitwise(BoolAndBatchOperator::bmtCount(cnt, 1));
        bmts.addBitwise(BoolAndBatchOperator::bmtCount(cnt, 1));
        return bmts;
    };
    auto valids = runBatches(plan, prepare, [&](int b, size_t begin, size_t end, BmtList& bmts) {
        const size_t cnt = end - begin;
        Shares cmp0(cnt), cmp1(cnt), lval(cnt), rval(cnt);
        size_t i = begin / rows1, j = begin % rows1;
        for (size_t off = 0; off < cnt; ++off) {
            cmp0[off] = col0[i];
            cmp1[off] = col1[j];
            lval[off] = lvalid[i];
            rval[off] = rvalid[j];
            if (++j == rows1) {
                j = 0;
                ++i;
            }
        }

        const int baseTag = b * blockStride;
        auto eqRes = equalOp(cmp0, cmp1, keyWidth, baseTag, bmts.nextBitwise(), JOIN_TASK_TAG);
        auto pairValid = andOp(lval, rval, baseTag + eqStride, bmts.nextBitwise(), JOIN_TASK_TAG);
        return andOp(eqRes, pairValid, baseTag + eqStride + andStride, bmts.nextBitwise(), JOIN_TASK_TAG);
    });
    joined._dataCols[joined.colNum() + View::VALID_COL_OFFSET] = concat(std::move(valids));

    if (compress && !BASELINE) {
        joined.clearInvalidEntries(foregroundTagBase());
    }

    return joined;
}

View Views::nestedLoopJoin(View& v0, View& v1, std::string& field0, std::string& field1) {
    return nestedLoopJoin(v0, v1, field0, field1, true);
}

std::vector<std::vector<std::vector<int64_t>>> Views::butterflyPermutation(View& view, int tagColIndex,
                                                                           int msgTagBase) {
    int numBuckets = DbConf::SHUFFLE_BUCKET_NUM;
    int numLayers = static_cast<int>(std::log2(numBuckets));

    std::vector<std::vector<std::vector<int64_t>>> buckets(numBuckets);

    // Uniformly distribute rows across all buckets
    int rowNum = view.rowNum();

    // Initialize all buckets
    for (int b = 0; b < numBuckets; b++) {
        buckets[b].reserve(view.colNum() - 1);
        for (int col = 0; col < view.colNum() - 1; col++) {
            buckets[b].push_back(std::vector<int64_t>());
        }
    }

    // Distribute rows uniformly across buckets
    for (int col = 0; col < view.colNum() - 1; col++) {
        for (int row = 0; row < rowNum; row++) {
            int bucketIdx = row % numBuckets; // Uniform distribution
            buckets[bucketIdx][col].push_back(view._dataCols[col][row]);
        }
    }

    int reserved = numBuckets / 2;
    for (int layer = 0; layer < numLayers; layer++) {
        int stride = 1 << layer;

        std::vector<int> totalRowsV;
        std::vector<int> indices1;
        std::vector<int> indices2;
        totalRowsV.reserve(reserved);
        indices1.reserve(reserved * view.rowNum());
        indices2.reserve(reserved * view.rowNum());

        std::vector<int64_t> mergedDatas;
        mergedDatas.reserve((view.colNum() * view.rowNum() * reserved));

        std::vector<int64_t> dummyDatas;

        std::vector<int64_t> routingBits;
        routingBits.reserve((view.colNum() * view.rowNum() * reserved));

        for (int i = 0; i < numBuckets; i += stride * 2) {
            for (int j = 0; j < stride; j++) {
                int idx1 = i + j;
                int idx2 = i + j + stride;
                std::vector<std::vector<int64_t>>& bucket1 = buckets[idx1];
                std::vector<std::vector<int64_t>>& bucket2 = buckets[idx2];

                if (idx2 < numBuckets) {
                    int bitPosition = numLayers - layer - 1;
                    size_t rows1 = bucket1.empty() ? 0 : bucket1[0].size();
                    size_t rows2 = bucket2.empty() ? 0 : bucket2[0].size();
                    size_t totalRows = rows1 + rows2;

                    if (totalRows == 0) {
                        continue;
                    }

                    totalRowsV.push_back(totalRows);
                    indices1.push_back(idx1);
                    indices2.push_back(idx2);

                    for (int col = 0; col < bucket1.size(); col++) {
                        for (size_t r = 0; r < rows1; r++) {
                            routingBits.push_back(Math::getBit(bucket1[tagColIndex][r], bitPosition));
                        }

                        for (size_t r = 0; r < rows2; r++) {
                            routingBits.push_back(Math::getBit(bucket2[tagColIndex][r], bitPosition));
                        }

                        if (!bucket1.empty()) {
                            mergedDatas.insert(mergedDatas.end(), bucket1[col].begin(), bucket1[col].end());
                        }
                        if (!bucket2.empty()) {
                            mergedDatas.insert(mergedDatas.end(), bucket2[col].begin(), bucket2[col].end());
                        }

                        dummyDatas.resize(mergedDatas.size(), 0);
                    }
                }
            }
        }

        // Routes every element by its bit: the first half of the result holds the lower bucket's new contents, the
        // second half the upper bucket's.
        std::vector<int64_t> allResults;
        const BatchPlan plan = planBatches(mergedDatas.size());
        auto prepare = [&](int, size_t begin, size_t end) {
            BmtList bmts(prefetchBmts(plan.parallel));
            bmts.addBitwise(BoolMutexBatchOperator::bmtCount(static_cast<int>(end - begin), view._maxWidth));
            return bmts;
        };
        auto parts = runBatches(plan, prepare, [&](int b, size_t begin, size_t end, BmtList& bmts) {
            Shares bufData, bufDummy, bufRouting;
            return mutexBothOp(*slice(mergedDatas, begin, end, bufData), *slice(dummyDatas, begin, end, bufDummy),
                               *slice(routingBits, begin, end, bufRouting), view._maxWidth,
                               msgTagBase + BoolMutexBatchOperator::tagStride() * b, bmts.nextBitwise());
        });
        if (parts.size() == 1) {
            allResults = std::move(parts[0]);
        } else {
            const size_t totalCnt = mergedDatas.size();
            allResults.resize(totalCnt * 2);
            size_t frontPos = 0;
            for (auto& part : parts) {
                const size_t subHalf = part.size() / 2;
                std::copy_n(part.begin(), subHalf, allResults.begin() + frontPos);
                std::copy_n(part.begin() + subHalf, subHalf, allResults.begin() + totalCnt + frontPos);
                frontPos += subHalf;
            }
        }

        int resultIndex = 0;
        int half = static_cast<int>(allResults.size() / 2);
        for (int i = 0; i < indices1.size(); i++) {
            if (totalRowsV[i] == 0) {
                continue;
            }
            std::vector<std::vector<int64_t>>& bucket1 = buckets[indices1[i]];
            std::vector<std::vector<int64_t>>& bucket2 = buckets[indices2[i]];
            std::vector<std::vector<int64_t>> newBucket1;
            std::vector<std::vector<int64_t>> newBucket2;

            for (int col = 0; col < bucket1.size(); col++) {
                auto data1Start = allResults.begin() + resultIndex;
                auto data1End = data1Start + totalRowsV[i];
                auto data2Start = data1Start + half;
                auto data2End = data2Start + totalRowsV[i];
                auto bucket1Data = std::vector(data1Start, data1End);
                auto bucket2Data = std::vector(data2Start, data2End);

                newBucket1.push_back(std::move(bucket1Data));
                newBucket2.push_back(std::move(bucket2Data));
                resultIndex += totalRowsV[i];
            }

            View temp1(view._tableName, view._fieldNames, view._fieldWidths, false);
            View temp2(view._tableName, view._fieldNames, view._fieldWidths, false);
            temp1._dataCols = std::move(newBucket1);
            temp1._dataCols.emplace_back(temp1.rowNum(), 0);
            temp2._dataCols = std::move(newBucket2);
            temp2._dataCols.emplace_back(temp2.rowNum(), 0);
            temp1.clearInvalidEntries(msgTagBase);
            temp2.clearInvalidEntries(msgTagBase);

            newBucket1 = std::move(temp1._dataCols);
            newBucket2 = std::move(temp2._dataCols);
            newBucket1.resize(newBucket1.size() - 1);
            newBucket2.resize(newBucket2.size() - 1);

            bucket1 = std::move(newBucket1);
            bucket2 = std::move(newBucket2);
        }
    }

    return buckets;
}

View Views::performBucketJoins(std::vector<std::vector<std::vector<int64_t>>>& buckets0,
                               std::vector<std::vector<std::vector<int64_t>>>& buckets1, View& v0, View& v1,
                               std::string& field0, std::string& field1, bool compress) {
    const size_t numBuckets = buckets0.size();
    bool firstResult = true;
    View result;

    for (size_t b = 0; b < numBuckets; ++b) {
        if (buckets0[b].empty() || buckets1[b].empty() || buckets0[b][0].empty() || buckets1[b][0].empty()) {
            continue;
        }

        std::vector<std::string> names0 = v0._fieldNames;
        std::vector<int> widths0 = v0._fieldWidths;
        View left(v0._tableName, v0._fieldNames, v0._fieldWidths, false);
        left._dataCols = buckets0[b];
        left._dataCols.emplace_back(left.rowNum(), 0);

        std::vector<std::string> names1 = v1._fieldNames;
        std::vector<int> widths1 = v1._fieldWidths;
        View right(v1._tableName, v1._fieldNames, v1._fieldWidths, false);
        right._dataCols = buckets1[b];
        right._dataCols.emplace_back(right.rowNum(), 0);

        View joined = nestedLoopJoin(left, right, field0, field1, compress);

        if (joined._dataCols.empty() || joined._dataCols[0].empty()) {
            continue;
        }

        if (firstResult) {
            result = joined;
            firstResult = false;
        } else {
            for (size_t col = 0; col < result._dataCols.size(); ++col) {
                result._dataCols[col].insert(result._dataCols[col].end(), joined._dataCols[col].begin(),
                                             joined._dataCols[col].end());
            }
        }
    }

    if (firstResult) {
        std::vector<std::string> fieldNames;
        std::vector<int> fieldWidths;
        size_t eff0 = v0.colNum() - 2;
        size_t eff1 = v1.colNum() - 2;

        fieldNames.reserve(eff0 + eff1);
        fieldWidths.reserve(eff0 + eff1);

        std::string t0 = v0._tableName.empty() ? "$t0" : v0._tableName;
        std::string t1 = v1._tableName.empty() ? "$t1" : v1._tableName;

        for (size_t i = 0; i < eff0; ++i) {
            fieldNames.emplace_back(t0 + "." + v0._fieldNames[i]);
            fieldWidths.emplace_back(v0._fieldWidths[i]);
        }
        for (size_t i = 0; i < eff1; ++i) {
            fieldNames.emplace_back(t1 + "." + v1._fieldNames[i]);
            fieldWidths.emplace_back(v1._fieldWidths[i]);
        }

        result = View(fieldNames, fieldWidths);
    }

    return result;
}

int Views::butterflyPermutationTagStride(View& v) {
    size_t totalCount = v.colNum() * v.rowNum() * DbConf::SHUFFLE_BUCKET_NUM / 2;
    return static_cast<int>((totalCount + Conf::BATCH_SIZE - 1) / Conf::BATCH_SIZE) *
           BoolMutexBatchOperator::tagStride();
}

View Views::hashJoin(View& v0, View& v1, std::string& field0, std::string& field1) {
    return hashJoin(v0, v1, field0, field1, true);
}

View Views::hashJoin(View& v0, View& v1, std::string& field0, std::string& field1, bool compress) {
    if (DbConf::BASELINE_MODE) {
        return nestedLoopJoin(v0, v1, field0, field1);
    }

    int numBuckets = DbConf::SHUFFLE_BUCKET_NUM;

    int tagColIndex0 = -1, tagColIndex1 = -1;

    for (int i = 0; i < v0._fieldNames.size(); i++) {
        if (v0._fieldNames[i].find(Table::BUCKET_TAG_PREFIX) == 0) {
            tagColIndex0 = i;
            break;
        }
    }

    for (int i = 0; i < v1._fieldNames.size(); i++) {
        if (v1._fieldNames[i].find(Table::BUCKET_TAG_PREFIX) == 0) {
            tagColIndex1 = i;
            break;
        }
    }

    if (tagColIndex0 == -1 || tagColIndex1 == -1) {
        return nestedLoopJoin(v0, v1, field0, field1, compress);
    }

    std::vector<std::vector<std::vector<int64_t>>> buckets0, buckets1;
    buckets0 = butterflyPermutation(v0, tagColIndex0, foregroundTagBase());
    buckets1 = butterflyPermutation(v1, tagColIndex1, foregroundTagBase());

    for (int i = 0; i < numBuckets; i++) {
        if (!buckets0[i].empty() && !buckets1[i].empty() && !buckets0[i][0].empty() && !buckets1[i][0].empty()) {
        }
    }

    auto result = performBucketJoins(buckets0, buckets1, v0, v1, field0, field1, compress);

    return result;
}

View Views::leftOuterJoin(View& v0, View& v1, std::string& field0, std::string& field1) {
    return leftOuterJoin(v0, v1, field0, field1, true, true);
}

View Views::leftOuterJoin(View& v0, View& v1, std::string& field0, std::string& field1, bool doHashJoin,
                          bool compress) {
    View joined;
    if (doHashJoin) {
        joined = hashJoin(v0, v1, field0, field1, compress);
    } else {
        joined = nestedLoopJoin(v0, v1, field0, field1, compress);
    }

    const int k0 = v0.colIndex(field0);
    const int k1 = v1.colIndex(field1);
    if (k0 < 0 || k1 < 0)
        return joined;

    const auto& left_keys = v0._dataCols[k0];
    const auto& right_keys = v1._dataCols[k1];

    const int v0_valid_idx = v0.colNum() + View::VALID_COL_OFFSET;
    const int v1_valid_idx = v1.colNum() + View::VALID_COL_OFFSET;
    const auto& left_valid = v0._dataCols[v0_valid_idx];

    std::vector<int64_t> has_match =
        in(const_cast<std::vector<int64_t>&>(left_keys), const_cast<std::vector<int64_t>&>(right_keys),
           const_cast<std::vector<int64_t>&>(v0._dataCols[v0_valid_idx]),
           const_cast<std::vector<int64_t>&>(v1._dataCols[v1_valid_idx]));

    const size_t eff0 = v0.colNum() - 2;
    const size_t eff1 = v1.colNum() - 2;

    std::vector<std::string> names;
    names.reserve(eff0 + eff1);
    std::vector<int> widths;
    widths.reserve(eff0 + eff1);

    for (size_t i = 0; i < eff0; ++i) {
        names.push_back(v0._fieldNames[i]);
        widths.push_back(v0._fieldWidths[i]);
    }
    for (size_t i = 0; i < eff1; ++i) {
        names.push_back(v1._fieldNames[i]);
        widths.push_back(v1._fieldWidths[i]);
    }

    View left_only(names, widths);
    const size_t n0 = v0.rowNum();

    for (size_t c = 0; c < eff0; ++c) {
        left_only._dataCols[c] = v0._dataCols[c];
    }
    for (size_t c = 0; c < eff1; ++c) {
        left_only._dataCols[eff0 + c].assign(n0, 0);
    }

    std::vector<int64_t> not_has(n0);
    const int64_t MASK_ONE = Comm::rank();
    for (size_t i = 0; i < n0; ++i)
        not_has[i] = has_match[i] ^ MASK_ONE;
    not_has = BoolAndBatchOperator(&not_has, const_cast<std::vector<int64_t>*>(&left_valid), 1, 0, foregroundTagBase(),
                                   SecureOperator::NO_CLIENT_COMPUTE)
                  .execute()
                  ->_zis;

    {
        const int insertPos = left_only.colNum() - 2;
        left_only._fieldNames.insert(left_only._fieldNames.begin() + insertPos,
                                     View::OUTER_MATCH_PREFIX + v1._tableName);
        left_only._fieldWidths.insert(left_only._fieldWidths.begin() + insertPos, 1);
        left_only._dataCols.insert(left_only._dataCols.begin() + insertPos, std::vector<int64_t>(n0, 0));
    }

    left_only._dataCols[left_only.colNum() + View::VALID_COL_OFFSET] = std::move(not_has);
    left_only._dataCols[left_only.colNum() + View::PADDING_COL_OFFSET] = std::vector<int64_t>(left_only.rowNum(), 0);

    {
        const int insertPos = joined.colNum() - 2;
        const int j_valid = joined.colNum() + View::VALID_COL_OFFSET;
        std::vector<int64_t> match_joined = joined._dataCols[j_valid];

        joined._fieldNames.insert(joined._fieldNames.begin() + insertPos, View::OUTER_MATCH_PREFIX + v1._tableName);
        joined._fieldWidths.insert(joined._fieldWidths.begin() + insertPos, 1);
        joined._dataCols.insert(joined._dataCols.begin() + insertPos, std::move(match_joined));
    }

    if (!DbConf::BASELINE_MODE && compress) {
        left_only.clearInvalidEntries(foregroundTagBase());
    }

    const size_t total_cols = joined.colNum();
    for (size_t col = 0; col < total_cols; ++col) {
        auto& dst = joined._dataCols[col];
        auto& src = left_only._dataCols[col];
        dst.insert(dst.end(), src.begin(), src.end());
    }

    return joined;
}

std::string Views::getAliasColName(std::string& tableName, std::string& fieldName) {
    return tableName + "." + fieldName;
}

int64_t Views::hash(int64_t keyValue) { return (keyValue * 31 + 17) % DbConf::SHUFFLE_BUCKET_NUM; }

std::vector<int64_t> Views::in(std::vector<int64_t>& col1, std::vector<int64_t>& col2, std::vector<int64_t>& left_valid,
                               std::vector<int64_t>& right_valid) {
    const size_t n = col1.size();
    const size_t m = col2.size();
    std::vector<int64_t> result(n, 0);
    if (n == 0 || m == 0)
        return result;

    const int64_t rankShare = Comm::rank();
    // The message tags below carry no task prefix; run them in the foreground task, apart from the background BMT
    // generators, which own the lowest task tags.
    const int taskTag = System::nextTask();

    size_t cols = m;
    int reduceRounds = 0;
    while (cols > 1) {
        cols = cols / 2 + (cols & 1);
        ++reduceRounds;
    }

    const int eqStride = BoolEqualBatchOperator::tagStride();
    const int andStride = BoolAndBatchOperator::tagStride();

    const bool maskRight = right_valid.size() == m;
    if (!maskRight) {
        Log::e("in: right_valid size mismatch: got {}, expect {}", right_valid.size(), m);
    }

    // Entry i * m + j of the n x m matrix holds NOT(col1[i] == col2[j] AND right_valid[j]).
    const int eqBlockStride = eqStride + andStride;
    const BatchPlan eqPlan = planBatches(n * m);
    auto prepareEq = [&](int, size_t begin, size_t end) {
        BmtList bmts(prefetchBmts(eqPlan.parallel));
        const int cnt = static_cast<int>(end - begin);
        bmts.addBitwise(BoolEqualBatchOperator::bmtCount(cnt, 64));
        if (maskRight) {
            bmts.addBitwise(BoolAndBatchOperator::bmtCount(cnt, 1));
        }
        return bmts;
    };
    std::vector<int64_t> cur =
        concat(runBatches(eqPlan, prepareEq, [&](int b, size_t begin, size_t end, BmtList& bmts) {
            const size_t cnt = end - begin;
            Shares left(cnt), right(cnt), rv(maskRight ? cnt : 0);
            size_t i = begin / m, j = begin % m;
            for (size_t off = 0; off < cnt; ++off) {
                left[off] = col1[i];
                right[off] = col2[j];
                if (maskRight) {
                    rv[off] = right_valid[j];
                }
                if (++j == m) {
                    j = 0;
                    ++i;
                }
            }
            const int eqTag = b * eqBlockStride;
            auto eqRes = equalOp(left, right, 64, eqTag, bmts.nextBitwise(), taskTag);
            if (maskRight) {
                eqRes = andOp(eqRes, rv, eqTag + eqStride, bmts.nextBitwise(), taskTag);
            }
            flipBits(eqRes);
            return eqRes;
        }));

    // AND-reduce every row pairwise; a row has a match iff not all of its entries are true.
    const int reduceTagBase = eqPlan.numBatches * eqBlockStride;
    size_t curCols = m;
    int round = 0;
    while (curCols > 1) {
        const size_t pairsPerRow = curCols / 2;
        const bool hasOdd = (curCols & 1);
        const size_t totalPairs = n * pairsPerRow;

        const BatchPlan plan = planBatches(totalPairs);
        auto prepareAnd = [&](int, size_t begin, size_t end) {
            BmtList bmts(prefetchBmts(plan.parallel));
            bmts.addBitwise(BoolAndBatchOperator::bmtCount(static_cast<int>(end - begin), 1));
            return bmts;
        };
        auto andOut = concat(runBatches(plan, prepareAnd, [&](int b, size_t begin, size_t end, BmtList& bmts) {
            const size_t cnt = end - begin;
            Shares andLeft(cnt), andRight(cnt);
            for (size_t t = 0; t < cnt; ++t) {
                const size_t g = begin + t;
                const size_t base = (g / pairsPerRow) * curCols + 2 * (g % pairsPerRow);
                andLeft[t] = cur[base];
                andRight[t] = cur[base + 1];
            }
            return andOp(andLeft, andRight, reduceTagBase + (round + b) * andStride, bmts.nextBitwise(), taskTag);
        }));

        const size_t nextCols = pairsPerRow + (hasOdd ? 1 : 0);
        std::vector<int64_t> next(n * nextCols);
        size_t w = 0;
        for (size_t i = 0; i < n; ++i) {
            for (size_t p = 0; p < pairsPerRow; ++p) {
                next[i * nextCols + p] = andOut[w++];
            }
            if (hasOdd) {
                next[i * nextCols + pairsPerRow] = cur[i * curCols + (curCols - 1)];
            }
        }
        cur.swap(next);
        curCols = nextCols;
        ++round;
    }

    for (size_t i = 0; i < n; ++i)
        result[i] = cur[i] ^ rankShare;

    if (left_valid.size() == n) {
        const int finalMaskTag = eqPlan.numBatches * (eqStride + andStride) + reduceRounds * andStride;
        result = andOp(result, left_valid, finalMaskTag, nullptr, taskTag);
    } else {
        Log::e("in: left_valid size mismatch: got {}, expect {}", left_valid.size(), n);
    }

    return result;
}

static std::string makeBorder(const std::vector<size_t>& w) {
    std::ostringstream oss;
    oss << '+';
    for (auto width : w) {
        oss << std::string(width + 2, '-') << '+';
    }
    return oss.str();
}

static void writeCell(std::ostringstream& oss, const std::string& s, size_t width) {
    oss << ' ' << s;
    if (s.size() < width)
        oss << std::string(width - s.size(), ' ');
    oss << ' ';
}

void Views::revealAndPrint(View& v) {
    const int C = v.colNum();
    if (C == 0) {
        Log::i("(0 columns)");
        return;
    }

    const bool isSender = (Comm::rank() == 1);
    // Direct Comm calls bypass SecureOperator::buildTag(): keep them out of the task tags of the background BMT
    // generators, whose traffic a reveal on tag 0 could otherwise consume.
    const int revealTagBase = System::nextTask() << (32 - Conf::TASK_TAG_BITS);

    std::vector<std::vector<int64_t>> plainCols(C);
    size_t nRows = 0;

    for (int i = 0; i < C; i++) {
        if (isSender) {
            Comm::serverSend(v._dataCols[i], 64, revealTagBase + i);
        } else {
            std::vector<int64_t> other;
            Comm::serverReceive(other, 64, revealTagBase + i);
            const auto& mine = v._dataCols[i];
            std::vector<int64_t> merged;
            merged.resize(mine.size());
            for (size_t j = 0; j < mine.size(); j++) {
                merged[j] = mine[j] ^ other[j];
            }
            plainCols[i] = std::move(merged);
            if (i == 0)
                nRows = plainCols[i].size();
        }
    }

    if (isSender) {
        return;
    }

    std::vector<size_t> colWidth(C);
    for (int i = 0; i < C; i++) {
        size_t w = v._fieldNames[i].size();
        const auto& col = plainCols[i];
        for (size_t r = 0; r < col.size(); r++) {
            const std::string s = std::to_string(col[r]);
            w = std::max(w, s.size());
        }
        colWidth[i] = w;
    }

    std::ostringstream out;
    const std::string top = makeBorder(colWidth);
    out << top << '\n' << '|';
    for (int i = 0; i < C; i++) {
        writeCell(out, v._fieldNames[i], colWidth[i]);
        out << '|';
    }
    out << '\n' << top << '\n';

    for (size_t r = 0; r < nRows; r++) {
        out << '|';
        for (int c = 0; c < C; c++) {
            const std::string cell = std::to_string(plainCols[c][r]);
            writeCell(out, cell, colWidth[c]);
            out << '|';
        }
        out << '\n';
    }
    out << top << '\n';
    out << "(" << nRows << (nRows == 1 ? " row)" : " rows)") << '\n';

    Log::i("\n{}", out.str());
}

void Views::addRedundantCols(View& v) {
    v._fieldNames.emplace_back(View::VALID_COL_NAME);
    v._fieldWidths.emplace_back(1);
    v._dataCols.emplace_back(v._dataCols[0].size());

    v._fieldNames.emplace_back(View::PADDING_COL_NAME);
    v._fieldWidths.emplace_back(1);
    v._dataCols.emplace_back(v._dataCols[0].size());
}

} // namespace parsecdb
