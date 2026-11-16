
#ifndef VIEW_H
#define VIEW_H
#include "parsecdb/db/Table.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace parsecdb {

namespace db {
class BmtList;
}

class View : public Table {
  public:
    static const int VALID_COL_OFFSET = -2;
    static const int PADDING_COL_OFFSET = -1;
    inline static const std::string VALID_COL_NAME = "$valid";
    inline static const std::string PADDING_COL_NAME = "$padding";
    inline static const std::string COUNT_COL_NAME = "$cnt";
    inline static const std::string OUTER_MATCH_PREFIX = "$match:";

    inline static std::string EMPTY_KEY_FIELD;
    inline static std::string EMPTY_VIEW_NAME;

    enum ComparatorType {
        LESS,
        GREATER,
        GREATER_EQ,
        LESS_EQ,
        EQUALS,
        NOT_EQUALS,
    };

    View() = default;

    View(std::vector<std::string>& fieldNames, std::vector<int>& fieldWidths);

    View(std::string& tableName, std::vector<std::string>& fieldNames, std::vector<int>& fieldWidths);

    View(std::string& tableName, std::vector<std::string>& fieldNames, std::vector<int>& fieldWidths, bool dummy);

    void select(std::vector<std::string>& fieldNames);

    void sort(const std::string& orderField, bool ascendingOrder, int msgTagBase);

    void sort(const std::vector<std::string>& orderFields, const std::vector<bool>& ascendingOrders, int msgTagBase);

    int sortTagStride();

    int sortTagStride(const std::vector<std::string>& orderFields);

    void filterAndConditions(std::vector<std::string>& fieldNames, std::vector<ComparatorType>& comparatorTypes,
                             std::vector<int64_t>& constShares, bool clear, int msgTagBase);

    void filterAndConditions(std::vector<std::string>& fieldNames, std::vector<ComparatorType>& comparatorTypes,
                             std::vector<int64_t>& constShares, int msgTagBase);

    void clearInvalidEntries(int msgTagBase);

    void clearInvalidEntries(bool doSort, int msgTagBase);

    int clearInvalidEntriesTagStride();

    void addRedundantCols();

    std::vector<int64_t> groupBy(const std::string& groupField, int msgTagBase);

    std::vector<int64_t> groupBy(const std::string& groupField, bool doSort, int msgTagBase);

    std::vector<int64_t> groupBy(const std::vector<std::string>& groupFields, int msgTagBase);

    void count(std::vector<std::string>& groupFields, std::vector<int64_t>& heads, std::string alias, int msgTagBase);

    void count(std::vector<std::string>& groupFields, std::vector<int64_t>& heads, std::string alias, bool compress,
               int msgTagBase);

    void max(std::vector<int64_t>& heads, const std::string& fieldName, std::string alias, int msgTagBase);

    void min(std::vector<int64_t>& heads, const std::string& fieldName, std::string alias, int msgTagBase);

    void minAndMax(std::vector<int64_t>& heads, const std::string& fieldName, std::string minAlias,
                   std::string maxAlias, int msgTagBase);

    void minAndMax(std::vector<int64_t>& heads, const std::string& minFieldName, const std::string& maxFieldName,
                   std::string minAlias, std::string maxAlias, int msgTagBase);

    void distinct(int msgTagBase);

    int groupByTagStride();

    int distinctTagStride();

  private:
    // Polls, into the list, the BMTs that SwapBitsFn consumes for a batch of cnt compare-exchange pairs.
    using PrepareSwapBitsFn = std::function<void(int cnt, db::BmtList& bmts)>;

    // Computes the swap bit of the compare-exchange pairs [begin, end) of one bitonic step.
    using SwapBitsFn = std::function<std::vector<int64_t>(const std::vector<int>& xIdx, const std::vector<int>& yIdx,
                                                          const std::vector<bool>& dirs, size_t begin, size_t end,
                                                          int msgTagBase, db::BmtList& bmts)>;

    // One aggregate of groupExtremes(): the running max or min of a column.
    struct ExtremeSpec {
        int srcIdx;
        int width;
        bool isMax;
        std::string outName;
    };

    void bitonicSort(const std::string& orderField, bool ascendingOrder, int msgTagBase);

    void bitonicSort(const std::vector<std::string>& orderFields, const std::vector<bool>& ascendingOrders,
                     int msgTagBase);

    void bitonicNetwork(int batchTagStride, int msgTagBase, const PrepareSwapBitsFn& prepareSwapBits,
                        const SwapBitsFn& swapBits);

    void swapRows(const std::vector<int>& xIdx, const std::vector<int>& yIdx, size_t begin, size_t end,
                  std::vector<int64_t>& swapBits, bool perColumn, int msgTagBase, db::BmtList& bmts);

    std::vector<int64_t> groupHeads(const std::vector<int>& groupCols, int tagOffset, int msgTagBase);

    std::vector<int64_t> groupTails(const std::vector<int64_t>& heads, int msgTagBase);

    void applyGroupTails(int validIdx, std::vector<int64_t>& tails, bool compress, int msgTagBase);

    void countSorted(std::vector<int64_t>& heads, const std::string& alias, const std::string& matchedTable,
                     bool compress, int msgTagBase);

    void groupExtremes(std::vector<int64_t>& heads, const std::vector<ExtremeSpec>& specs, bool legacyValidIdx,
                       int msgTagBase);
};

} // namespace parsecdb

#endif
