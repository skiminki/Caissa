#pragma once

#include "Position.hpp"
#include "Move.hpp"

#include <unordered_map>



struct TranspositionTableEntry
{
    enum Flags : uint8_t
    {
        Flag_Invalid,
        Flag_Exact,
        Flag_LowerBound,
        Flag_UpperBound,
    };

    uint64_t positionHash;
    int32_t score = INT32_MIN;
    PackedMove move;
    uint8_t depth = 0;
    Flags flag = Flag_Invalid;
};

static_assert(sizeof(TranspositionTableEntry) == 16, "TT entry is too big");

struct SearchParam
{
    uint32_t transpositionTableSize = 32 * 1024 * 1024;
    uint8_t maxDepth = 8;
    bool debugLog = true;
};

class Search
{
public:

    using ScoreType = int32_t;
    static constexpr int32_t CheckmateValue = 100000;
    static constexpr int32_t InfValue       = 10000000;

    static constexpr int32_t MaxSearchDepth = 64;

    Search();

    ScoreType DoSearch(const Position& position, Move& outBestMove, const SearchParam& param);

    void RecordBoardPosition(const Position& position);

    // check if a position was repeated 2 times
    bool IsPositionRepeated(const Position& position, uint32_t repetitionCount = 2u) const;

private:

    Search(const Search&) = delete;

    struct NodeInfo
    {
        const Position* position = nullptr;
        const NodeInfo* parentNode = nullptr;
        uint8_t depth;
        uint8_t maxDepth;
        ScoreType alpha;
        ScoreType beta;
        Color color;
        bool isPvNode = false;
    };
    
    struct SearchContext
    {
        uint64_t fh = 0;
        uint64_t fhf = 0;
        uint64_t nodes = 0;
        uint64_t quiescenceNodes = 0;
        uint64_t pseudoMovesPerNode = 0;
        uint64_t ttHits = 0;
    };

    struct PvLineEntry
    {
        uint64_t positionHash;
        Move move;
    };

    struct GameHistoryPosition
    {
        Position pos;           // board position
        uint32_t count = 0;     // how many times it occurred during the game
    };

    using GameHistoryPositionEntry = std::vector<GameHistoryPosition>;

    // principial variation moves tracking for current search
    PackedMove pvArray[MaxSearchDepth][MaxSearchDepth];
    uint16_t pvLengths[MaxSearchDepth];

    // principial variation line from previous iterative deepening search
    uint16_t prevPvArrayLength;
    PvLineEntry prevPvArray[MaxSearchDepth];

    std::vector<TranspositionTableEntry> transpositionTable;

    uint32_t searchHistory[2][6][64];

    static constexpr uint32_t NumKillerMoves = 3;
    Move killerMoves[MaxSearchDepth][NumKillerMoves];

    std::unordered_map<uint64_t, GameHistoryPositionEntry> historyGamePositions;

    ScoreType QuiescenceNegaMax(const NodeInfo& node, SearchContext& ctx);
    ScoreType NegaMax(const NodeInfo& node, SearchContext& ctx);

    TranspositionTableEntry* ReadTranspositionTable(const Position& position);
    void WriteTranspositionTable(const TranspositionTableEntry& entry);
    void PrefetchTranspositionTableEntry(const Position& position) const;

    // check if one of generated moves is in PV table
    const Move FindPvMove(uint32_t depth, const uint64_t positionHash, MoveList& moves) const;
    void FindHistoryMoves(Color color, MoveList& moves) const;
    void FindKillerMoves(uint32_t depth, MoveList& moves) const;

    int32_t PruneByMateDistance(const NodeInfo& node, int32_t alpha, int32_t beta);

    // check for repetition in the searched node
    bool IsRepetition(const NodeInfo& node) const;

    // update principal variation line
    void UpdatePvArray(uint32_t depth, const Move move);

    void UpdateSearchHistory(const NodeInfo& node, const Move move);
};