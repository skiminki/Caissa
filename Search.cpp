#include "Search.hpp"
#include "MoveList.hpp"
#include "Evaluate.hpp"

#include <iostream>
#include <string>

static const uint32_t NullMovePrunningStartDepth = 3;
static const int32_t NullMovePrunningDepthReduction = 3;

static const uint32_t LateMoveReductionStartDepth = 3;
static const uint32_t LateMoveReductionRate = 8;

static const uint32_t LateMovePrunningStartDepth = 3;

static const uint32_t AspirationWindowSearchStartDepth = 4;
static const int32_t AspirationWindowMax = 200;
static const int32_t AspirationWindowMin = 20;
static const int32_t AspirationWindowStep = 20;

static const uint32_t BetaPruningDepth = 6;
static const int32_t BetaMarginMultiplier = 80;
static const int32_t BetaMarginBias = 30;

static const uint32_t AlphaPruningDepth = 4;
static const int32_t AlphaMarginMultiplier = 150;
static const int32_t AlphaMarginBias = 1000;

int64_t SearchParam::GetElapsedTime() const
{
    auto endTimePoint = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(endTimePoint - startTimePoint).count();
}

Search::Search()
{
#ifndef _DEBUG
    mTranspositionTable.Resize(8 * 1024 * 1024);
#else
    mTranspositionTable.Resize(1024 * 1024);
#endif
}

Search::~Search()
{

}

void Search::RecordBoardPosition(const Position& position)
{
    GameHistoryPositions& entry = historyGamePositions[position.GetHash()];

    for (const Position& historyPosition : entry)
    {
        if (historyPosition == position)
        {
            return;
        }
    }

    entry.push_back(position);
}

void Search::ClearPositionHistory()
{
    historyGamePositions.clear();
}

bool Search::IsPositionRepeated(const Position& position) const
{
    const auto iter = historyGamePositions.find(position.GetHash());
    if (iter == historyGamePositions.end())
    {
        return false;
    }

    const GameHistoryPositions& entry = iter->second;

    for (const Position& historyPosition : entry)
    {
        if (historyPosition == position)
        {
            return true;
        }
    }

    return false;
}

void Search::DoSearch(const Position& position, const SearchParam& param, SearchResult& result)
{
    std::vector<Move> pvMovesSoFar;

    mPrevPvLines.clear();

    // clamp number of PV lines (there can't be more than number of max moves)
    static_assert(MoveList::MaxMoves <= UINT8_MAX, "Max move count must fit uint8");
    const uint32_t numPvLines = std::min(param.numPvLines, position.GetNumLegalMoves());

    result.clear();
    result.resize(numPvLines);

    if (numPvLines == 0u)
    {
        // early exit in case of no legal moves
        return;
    }

    memset(searchHistory, 0, sizeof(searchHistory));
    memset(killerMoves, 0, sizeof(killerMoves));

    for (uint32_t depth = 1; depth <= param.maxDepth; ++depth)
    {
        pvMovesSoFar.clear();

        for (uint32_t pvIndex = 0; pvIndex < numPvLines; ++pvIndex)
        {
            PvLine& outPvLine = result[pvIndex];

            auto startTime = std::chrono::high_resolution_clock::now();

            SearchContext searchContext{ param };

            AspirationWindowSearchParam aspirationWindowSearchParam =
            {
                position,
                param,
                result,
                depth,
                pvIndex,
                searchContext,
                pvIndex > 0u ? pvMovesSoFar : std::span<const Move>(),
                outPvLine.score
            };

            int32_t score = AspirationWindowSearch(aspirationWindowSearchParam);

            const bool isMate = (score > CheckmateValue - MaxSearchDepth) || (score < -CheckmateValue + MaxSearchDepth);

            uint32_t pvLength = pvLengths[0];

            // write PV line into result struct
            outPvLine.score = score;
            if (pvLength > 0)
            {
                outPvLine.moves.clear();

                // reconstruct PV line
                Position iteratedPosition = position;
                for (uint32_t i = 0; i < pvLength; ++i)
                {
                    const Move move = iteratedPosition.MoveFromPacked(pvArray[0][i]);

                    // Note: move in transpostion table may be invalid due to hash collision
                    if (!move.IsValid()) break;
                    if (!iteratedPosition.DoMove(move)) break;
                    
                    outPvLine.moves.push_back(move);
                }

                ASSERT(!outPvLine.moves.empty());
                pvMovesSoFar.push_back(outPvLine.moves.front());
            }
            else
            {
                break;
            }

            auto endTime = std::chrono::high_resolution_clock::now();

            if (param.debugLog)
            {
                std::cout << "info";
                std::cout << " depth " << (uint32_t)depth;
                std::cout << " seldepth " << (uint32_t)searchContext.maxDepth;
                if (param.numPvLines > 1)
                {
                    std::cout << " multipv " << (pvIndex + 1);
                }
                std::cout << " time " << std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
                if (isMate)
                {
                    std::cout << " score mate " << (pvLength + 1) / 2;
                }
                else
                {
                    std::cout << " score cp " << score;
                }
                std::cout << " nodes " << searchContext.nodes;

                std::cout << " pv ";
                {
                    for (size_t i = 0; i < outPvLine.moves.size(); ++i)
                    {
                        const Move move = outPvLine.moves[i];
                        ASSERT(move.IsValid());
                        std::cout << move.ToString();
                        if (i + 1 < outPvLine.moves.size()) std::cout << ' ';
                    }
                }

                std::cout << std::endl;
            }
        }

        mPrevPvLines = result;

        if (param.maxTime < UINT32_MAX)
        {
            if (param.GetElapsedTime() > param.maxTime)
            {
                // time limit exceeded
                break;
            }
        }
    }
}

int32_t Search::AspirationWindowSearch(const AspirationWindowSearchParam& param)
{
    int32_t alpha = -InfValue;
    int32_t beta = InfValue;

    // decrease aspiration window with increasing depth
    int32_t aspirationWindow = AspirationWindowMax - (param.depth - AspirationWindowSearchStartDepth) * AspirationWindowStep;
    aspirationWindow = std::max<int32_t>(AspirationWindowMin, aspirationWindow);
    ASSERT(aspirationWindow > 0);

    // start applying aspiration window at given depth
    if (param.depth >= AspirationWindowSearchStartDepth)
    {
        alpha = std::max(param.previousScore - aspirationWindow, -InfValue);
        beta = std::min(param.previousScore + aspirationWindow, InfValue);
    }

    for (;;)
    {
        if (param.searchParam.printMoves)
        {
            std::cout << "aspiration window: " << alpha << "..." << beta << "\n";
        }

        memset(pvArray, 0, sizeof(pvArray));
        memset(pvLengths, 0, sizeof(pvLengths));

        NodeInfo rootNode;
        rootNode.position = &param.position;
        rootNode.depth = 0u;
        rootNode.isPvNode = true;
        rootNode.maxDepthFractional = param.depth << MaxDepthShift;
        rootNode.pvIndex = (uint8_t)param.pvIndex;
        rootNode.alpha = alpha;
        rootNode.beta = beta;
        rootNode.color = param.position.GetSideToMove();
        rootNode.rootMoves = param.searchParam.rootMoves;
        rootNode.moveFilter = param.moveFilter;

        ScoreType score = NegaMax(rootNode, param.searchContext);

        ASSERT(score >= -CheckmateValue && score <= CheckmateValue);

        // out of aspiration window, redo the search in wider score range
        if (score <= alpha || score >= beta)
        {
            alpha -= aspirationWindow;
            beta += aspirationWindow;
            aspirationWindow *= 2;
            continue;
        }

        return score;
    }
}

static INLINE int32_t ColorMultiplier(Color color)
{
    return color == Color::White ? 1 : -1;
}

const Move Search::FindPvMove(const NodeInfo& node, MoveList& moves) const
{
    if (!node.isPvNode || mPrevPvLines.empty())
    {
        return Move{};
    }

    const std::vector<Move>& pvLine = mPrevPvLines[node.pvIndex].moves;
    if (node.depth >= pvLine.size())
    {
        return Move{};
    }

    const Move pvMove = pvLine[node.depth];
    ASSERT(pvMove.IsValid());

    for (uint32_t i = 0; i < moves.numMoves; ++i)
    {
        if (pvMove.IsValid() && moves[i].move == pvMove)
        {
            moves[i].score = INT32_MAX;
            return pvMove;
        }
    }

    // no PV move found?
    //ASSERT(false);
    return pvMove;
}

void Search::FindHistoryMoves(Color color, MoveList& moves) const
{
    for (uint32_t i = 0; i < moves.numMoves; ++i)
    {
        const Move move = moves[i].move;
        ASSERT(move.IsValid());

        const uint32_t score = searchHistory[(uint32_t)color][move.fromSquare.Index()][move.toSquare.Index()];
        const int64_t finalScore = (int64_t)moves[i].score + score;
        moves[i].score = (int32_t)std::min<uint64_t>(finalScore, INT32_MAX);
    }
}

void Search::FindKillerMoves(uint32_t depth, MoveList& moves) const
{
    ASSERT(depth < MaxSearchDepth);

    for (uint32_t i = 0; i < moves.numMoves; ++i)
    {
        for (uint32_t j = 0; j < NumKillerMoves; ++j)
        {
            if (moves[i].move == killerMoves[depth][j])
            {
                moves[i].score += 100000 - j;
            }
        }
    }
}

void Search::UpdatePvArray(uint32_t depth, const Move move)
{
    const uint8_t childPvLength = pvLengths[depth + 1];
    pvArray[depth][depth] = move;
    for (uint32_t j = depth + 1; j < childPvLength; ++j)
    {
        pvArray[depth][j] = pvArray[depth + 1][j];
    }
    pvLengths[depth] = childPvLength;
}

void Search::UpdateSearchHistory(const NodeInfo& node, const Move move)
{
    if (move.isCapture)
    {
        return;
    }

    uint32_t& historyCounter = searchHistory[(uint32_t)node.color][move.fromSquare.Index()][move.toSquare.Index()];

    const uint64_t historyBonus = node.MaxDepth() - node.depth;

    const uint64_t newValue = std::min<uint64_t>(UINT32_MAX, (uint64_t)historyCounter + 1u + historyBonus * historyBonus);
    historyCounter = (uint32_t)newValue;
}

void Search::RegisterKillerMove(const NodeInfo& node, const Move move)
{
    if (move.isCapture)
    {
        return;
    }

    for (uint32_t j = NumKillerMoves; j-- > 1u; )
    {
        killerMoves[node.depth][j] = killerMoves[node.depth][j - 1];
    }
    killerMoves[node.depth][0] = move;
}

bool Search::IsRepetition(const NodeInfo& node) const
{
    for(const NodeInfo* prevNode = &node;;)
    {
        // only check every second previous node, because side to move must be the same
        if (prevNode->parentNode)
        {
            prevNode = prevNode->parentNode->parentNode;
        }
        else
        {
            prevNode = nullptr;
        }

        // reached end of the stack
        if (!prevNode)
        {
            break;
        }

        ASSERT(prevNode->position);
        if (prevNode->position->GetHash() == node.position->GetHash())
        {
            if (*prevNode->position == *node.position)
            {
                return true;
            }
        }
    }

    return IsPositionRepeated(*node.position);
}

bool Search::IsDraw(const NodeInfo& node) const
{
    if (node.position->GetHalfMoveCount() >= 100)
    {
        return true;
    }

    if (CheckInsufficientMaterial(*node.position))
    {
        return true;
    }

    if (IsRepetition(node))
    {
        return true;
    }

    return false;
}

Search::ScoreType Search::QuiescenceNegaMax(const NodeInfo& node, SearchContext& ctx)
{
    // clean PV line
    ASSERT(node.depth < MaxSearchDepth);
    pvLengths[node.depth] = (uint8_t)node.depth;

    // update stats
    ctx.nodes++;
    ctx.quiescenceNodes++;
    ctx.maxDepth = std::max<uint32_t>(ctx.maxDepth, node.depth);

    if (IsDraw(node))
    {
        return 0;
    }

    const ScoreType staticEval = ColorMultiplier(node.color) * Evaluate(*node.position);

    ScoreType score = staticEval;

    if (score >= node.beta)
    {
        return node.beta;
    }

    NodeInfo childNodeParam;
    childNodeParam.parentNode = &node;
    childNodeParam.depth = node.depth + 1;
    childNodeParam.maxDepthFractional = 0;
    childNodeParam.color = GetOppositeColor(node.color);

    uint32_t moveGenFlags = 0;
    if (!node.position->IsInCheck(node.color))
    {
        moveGenFlags |= MOVE_GEN_ONLY_TACTICAL;
    }

    MoveList moves;
    node.position->GenerateMoveList(moves, moveGenFlags);

    if (moves.numMoves > 1u)
    {
        FindPvMove(node, moves);
    }

    Move bestMove = Move::Invalid();
    ScoreType alpha = std::max(score, node.alpha);
    ScoreType beta = node.beta;
    uint32_t numLegalMoves = 0;

    for (uint32_t i = 0; i < moves.Size(); ++i)
    {
        int32_t moveScore = 0;
        const Move move = moves.PickBestMove(i, moveScore);

        Position childPosition = *node.position;
        if (!childPosition.DoMove(move))
        {
            continue;
        }

        numLegalMoves++;

        childNodeParam.position = &childPosition;
        childNodeParam.alpha = -beta;
        childNodeParam.beta = -alpha;
        score = -QuiescenceNegaMax(childNodeParam, ctx);

        if (score > alpha)
        {
            alpha = score;
            bestMove = move;

            //UpdatePvArray(node.depth, move);
        }

        if (score >= beta)
        {
            // for move ordering stats
            ctx.fh++;
            if (numLegalMoves == 1u) ctx.fhf++;
            return beta;
        }
    }
    
    if (node.MaxDepth() > 10 && !node.position->IsInCheck(node.color) && numLegalMoves == 0)
    {
        //std::cout << node.position->ToFEN() << " eval " << staticEval << std::endl;
        //std::cout << node.position->Print() << std::endl;
        //std::cout << node.position->ToFEN() << std::endl;
    }

    return alpha;
}


int32_t Search::PruneByMateDistance(const NodeInfo& node, int32_t alpha, int32_t beta)
{
    int32_t matingValue = CheckmateValue - node.depth;
    if (matingValue < beta)
    {
        beta = matingValue;
        if (alpha >= matingValue)
        {
            return matingValue;
        }
    }

    matingValue = -CheckmateValue + node.depth;
    if (matingValue > alpha)
    {
        alpha = matingValue;
        if (beta <= matingValue)
        {
            return matingValue;
        }
    }

    return 0;
}

Search::ScoreType Search::NegaMax(const NodeInfo& node, SearchContext& ctx)
{
    ASSERT(node.alpha <= node.beta);
    ASSERT(node.depth < MaxSearchDepth);

    // clean PV line
    pvLengths[node.depth] = (uint8_t)node.depth;

    // update stats
    ctx.nodes++;
    ctx.maxDepth = std::max<uint32_t>(ctx.maxDepth, node.depth);

    // root node is the first node in the chain (best move)
    const bool isRootNode = node.depth == 0;

    // Check for draw
    // Skip root node as we need some move to be reported
    if (!isRootNode)
    {
        if (IsDraw(node))
        {
            return 0;
        }
    }

    const bool isInCheck = node.position->IsInCheck(node.color);
    const uint32_t inversedDepth = node.MaxDepth() - node.depth;

    const ScoreType oldAlpha = node.alpha;
    ScoreType alpha = node.alpha;
    ScoreType beta = node.beta;

    // transposition table lookup
    PackedMove ttMove;
    ScoreType ttScore = InvalidValue;
    const TranspositionTableEntry* ttEntry = mTranspositionTable.Read(*node.position);

    if (ttEntry)
    {
        // always use hash move as a good first guess
        ttMove = ttEntry->move;

        const bool isFilteredMove = std::find(node.moveFilter.begin(), node.moveFilter.end(), ttEntry->move) != node.moveFilter.end();

        // TODO check if the move is valid move for current position
        // it maybe be not in case of rare hash collision

        if (ttEntry->depth >= inversedDepth && !isFilteredMove && !node.isPvNode)
        {
            ctx.ttHits++;

            if (ttEntry->flag == TranspositionTableEntry::Flag_Exact)
            {
                return ttEntry->score;
            }
            else if (ttEntry->flag == TranspositionTableEntry::Flag_LowerBound)
            {
                alpha = std::max(alpha, ttEntry->score);
            }
            else if (ttEntry->flag == TranspositionTableEntry::Flag_UpperBound)
            {
                beta = std::min(beta, ttEntry->score);
            }

            if (alpha >= beta)
            {
                return alpha;
            }

            ttScore = ttEntry->score;
        }
        else
        {
            ttEntry = nullptr;
        }
    }

    // mate distance prunning
    if (!isRootNode)
    {
        int32_t mateDistanceScore = PruneByMateDistance(node, alpha, beta);
        if (mateDistanceScore != 0)
        {
            return mateDistanceScore;
        }
    }


    // TODO endgame tables probing here


    // maximum search depth reached, enter quisence search to find final evaluation
    if (node.depth >= node.MaxDepth())
    {
        return QuiescenceNegaMax(node, ctx);
    }

    // Futility Pruning
    if (!node.isPvNode && !isInCheck)
    {
        // determine static evaluation of the board
        int32_t staticEvaluation = ttScore;
        if (staticEvaluation == InvalidValue)
        {
            staticEvaluation = ColorMultiplier(node.color) * Evaluate(*node.position);
        }

        const int32_t alphaMargin = AlphaMarginBias + AlphaMarginMultiplier * inversedDepth;
        const int32_t betaMargin = BetaMarginBias + BetaMarginMultiplier * inversedDepth;

        // Alpha Pruning
        if (inversedDepth <= AlphaPruningDepth && (staticEvaluation + alphaMargin <= alpha))
        {
            return staticEvaluation + alphaMargin;
        }

        // Beta Pruning
        if (inversedDepth <= BetaPruningDepth && (staticEvaluation - betaMargin >= beta))
        {
            return staticEvaluation - betaMargin;
        }
    }

    // Null Move Prunning
    if (!node.isPvNode && !isInCheck && inversedDepth >= NullMovePrunningStartDepth && ttScore >= beta && !ttMove.IsValid())
    {
        // don't allow null move if parent or grandparent node was null move
        bool doNullMove = !node.isNullMove;
        if (node.parentNode && node.parentNode->isNullMove)
        {
            doNullMove = false;
        }

        if (doNullMove)
        {
            Position childPosition = *node.position;
            childPosition.DoNullMove();

            NodeInfo childNodeParam;
            childNodeParam.parentNode = &node;
            childNodeParam.depth = node.depth + 1;
            childNodeParam.color = GetOppositeColor(node.color);
            childNodeParam.pvIndex = node.pvIndex;
            childNodeParam.position = &childPosition;
            childNodeParam.alpha = -beta;
            childNodeParam.beta = -beta + 1;
            childNodeParam.isNullMove = true;
            childNodeParam.maxDepthFractional = std::max(0, (int32_t)node.maxDepthFractional - (NullMovePrunningDepthReduction << MaxDepthShift));

            const int32_t nullMoveScore = -NegaMax(childNodeParam, ctx);

            if (nullMoveScore >= beta)
            {
                return beta;
            }
        }
    }

    NodeInfo childNodeParam;
    childNodeParam.parentNode = &node;
    childNodeParam.depth = node.depth + 1;
    childNodeParam.color = GetOppositeColor(node.color);
    childNodeParam.pvIndex = node.pvIndex;

    uint16_t extension = 0;

    // check extension
    if (isInCheck)
    {
        extension++;
    }

    MoveList moves;
    node.position->GenerateMoveList(moves);

    if (isRootNode)
    {
        // apply node filter (used for multi-PV search for 2nd, 3rd, etc. moves)
        if (!node.moveFilter.empty())
        {
            for (const Move& move : node.moveFilter)
            {
                moves.RemoveMove(move);
            }
        }

        // apply node filter (used for "searchmoves" UCI command)
        if (!node.rootMoves.empty())
        {
            // TODO
            //for (const Move& move : node.rootMoves)
            //{
            //    if (!moves.HasMove(move))
            //    {
            //        moves.RemoveMove(move);
            //    }
            //}
        }
    }

    ctx.pseudoMovesPerNode += moves.numMoves;

    const Move pvMove = FindPvMove(node, moves);

    if (moves.numMoves > 1u)
    {
        FindHistoryMoves(node.color, moves);
        FindKillerMoves(node.depth, moves);

        if (ttMove.IsValid())
        {
            for (uint32_t i = 0; i < moves.numMoves; ++i)
            {
                if (moves[i].move == ttMove)
                {
                    moves[i].score = INT32_MAX - 1;
                    break;
                }
            }
        }
    }

    if (isRootNode)
    {
        if (ctx.searchParam.printMoves)
        {
            moves.Print();
        }
    }

    Move bestMove = Move::Invalid();
    uint32_t numLegalMoves = 0;
    uint32_t numReducedMoves = 0;
    bool betaCutoff = false;

    // count (pseudo) quiet moves
    uint32_t totalQuietMoves = 0;
    for (uint32_t i = 0; i < moves.Size(); ++i)
    {
        const Move move = moves[i].move;
        if (move.IsQuiet())
        {
            totalQuietMoves++;
        }
    }

    for (uint32_t i = 0; i < moves.Size(); ++i)
    {
        int32_t moveScore = 0;
        const Move move = moves.PickBestMove(i, moveScore);
        ASSERT(move.IsValid());

        Position childPosition = *node.position;
        if (!childPosition.DoMove(move))
        {
            continue;
        }

        mTranspositionTable.Prefetch(childPosition);

        // store any best move, in case of we never improve alpha in this loop,
        // so we can write anything into transposition table
        if (numLegalMoves == 0)
        {
            bestMove = move;
        }

        numLegalMoves++;

        int32_t moveExtensionFractional = extension << MaxDepthShift;

        // recapture extension
        if (move.isCapture && node.previousMove.isCapture && move.toSquare == node.previousMove.toSquare)
        {
            //moveExtensionFractional += 1 << (MaxDepthShift - 1);
        }

        childNodeParam.position = &childPosition;
        childNodeParam.isPvNode = pvMove == move;
        childNodeParam.maxDepthFractional = node.maxDepthFractional + moveExtensionFractional;
        childNodeParam.previousMove = move;

        uint32_t depthReductionFractional = 0;

        // Late Move Reduction
        // don't reduce PV moves, while in check, captures, promotions, etc.
        if (move.IsQuiet() && !isInCheck && totalQuietMoves > 0 && numLegalMoves > 1u && inversedDepth >= LateMoveReductionStartDepth)
        {
            // reduce depth gradually
            //depthReductionFractional = (numReducedMoves << MaxDepthShift) / LateMoveReductionRate;
            depthReductionFractional = std::max<int32_t>(1, numReducedMoves / LateMoveReductionRate) << MaxDepthShift;
            numReducedMoves++;

            // Late Move Prunning
            if (inversedDepth >= LateMovePrunningStartDepth && depthReductionFractional > childNodeParam.maxDepthFractional)
            {
                continue;
            }
        }

        ASSERT(childNodeParam.maxDepthFractional >= depthReductionFractional);
        childNodeParam.maxDepthFractional = childNodeParam.maxDepthFractional - depthReductionFractional;

        ScoreType score;
        {
            if (numLegalMoves == 1)
            {
                childNodeParam.alpha = -beta;
                childNodeParam.beta = -alpha;
                score = -NegaMax(childNodeParam, ctx);
            }
            else // Principal Variation Search
            {
                childNodeParam.alpha = -alpha - 1;
                childNodeParam.beta = -alpha;
                score = -NegaMax(childNodeParam, ctx);

                if (score > alpha && score < beta)
                {
                    childNodeParam.alpha = -beta;
                    childNodeParam.beta = -alpha;
                    score = -NegaMax(childNodeParam, ctx);
                }
            }
        }

        // re-do search at full depth
        if (depthReductionFractional > 0 && score > alpha)
        {
            childNodeParam.maxDepthFractional = node.maxDepthFractional + moveExtensionFractional;
            childNodeParam.alpha = -beta;
            childNodeParam.beta = -alpha;
            score = -NegaMax(childNodeParam, ctx);
        }

        if (score > alpha) // new best move found
        {
            bestMove = move;
            alpha = score;

            UpdatePvArray(node.depth, move);
            UpdateSearchHistory(node, move);
        }

        if (score >= beta) // beta cutoff
        {
            // for move ordering stats
            ctx.fh++;
            if (numLegalMoves == 1u) ctx.fhf++;

            RegisterKillerMove(node, move);

            betaCutoff = true;
            break;
        }
    }

    if (numLegalMoves == 0u)
    {
        if (isInCheck) // checkmate
        {
            return -CheckmateValue + node.depth;
        }
        else // stalemate
        {
            return 0;
        }
    }

    ASSERT(bestMove.IsValid());

    // update transposition table
    {
        TranspositionTableEntry::Flags flag = TranspositionTableEntry::Flag_Exact;
        if (alpha <= oldAlpha)
        {
            flag = TranspositionTableEntry::Flag_UpperBound;
        }
        else if (betaCutoff)
        {
            flag = TranspositionTableEntry::Flag_LowerBound;
        }

        const TranspositionTableEntry entry{ node.position->GetHash(), alpha, bestMove, (uint8_t)inversedDepth, flag };

        mTranspositionTable.Write(entry);
    }

    ASSERT(alpha > -CheckmateValue && alpha < CheckmateValue);

    return alpha;
}