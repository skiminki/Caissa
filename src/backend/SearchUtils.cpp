#include "SearchUtils.hpp"
#include "Search.hpp"
#include "Position.hpp"
#include "PositionHash.hpp"
#include "Game.hpp"
#include "TranspositionTable.hpp"

// Upcoming repetition detection algorithm base on Stockfish implementation
// Algorithm by Marcel van Kervinck:
// http://www.open-chess.org/viewtopic.php?f=5&t=2300

constexpr uint32_t CuckooTableSize = 8192;

// First and second hash functions for indexing the cuckoo tables
INLINE uint32_t CuckooIndex1(uint64_t h) { return h % CuckooTableSize; }
INLINE uint32_t CuckooIndex2(uint64_t h) { return (h >> 16) % CuckooTableSize; }

// Cuckoo tables with Zobrist hashes of valid reversible moves, and the corresponding moves
static uint64_t gCuckooTable[CuckooTableSize];
static PackedMove gCuckooMoves[CuckooTableSize];

void SearchUtils::Init()
{
    memset(gCuckooTable, 0, sizeof(gCuckooTable));
    memset(gCuckooMoves, 0, sizeof(gCuckooMoves));

    uint32_t count = 0;
    for (const Color color : {Color::White, Color::Black})
    {
        // Note: pawn moves not included as they are not reversible
        for (const Piece piece : {Piece::Knight, Piece::Bishop, Piece::Rook, Piece::Queen, Piece::King})
        {
            for (uint32_t squareA = 0; squareA < 64; ++squareA)
            {
                for (uint32_t squareB = squareA + 1; squareB < 64; ++squareB)
                {
                    Bitboard attacks;
                    switch (piece)
                    {
                    case Piece::Pawn:   attacks = Bitboard::GetPawnAttacks(Square(squareA), color);     break;
                    case Piece::Knight: attacks = Bitboard::GetKnightAttacks(Square(squareA));          break;
                    case Piece::Bishop: attacks = Bitboard::GetBishopAttacks(Square(squareA));          break;
                    case Piece::Rook:   attacks = Bitboard::GetRookAttacks(Square(squareA));            break;
                    case Piece::Queen:  attacks = Bitboard::GetQueenAttacks(Square(squareA));           break;
                    case Piece::King:   attacks = Bitboard::GetKingAttacks(Square(squareA));            break;
                    }

                    if (attacks & Square(squareB).GetBitboard())
                    {
                        PackedMove move{ Square(squareA), Square(squareB) };
                        uint64_t key = GetPieceZobristHash(color, piece, squareA) ^ GetPieceZobristHash(color, piece, squareB) ^ GetSideToMoveZobristHash();
                        uint32_t index = CuckooIndex1(key);
                        for (;;)
                        {
                            std::swap(gCuckooTable[index], key);
                            std::swap(gCuckooMoves[index], move);
                            if (!move.IsValid()) break;
                            index = (index == CuckooIndex1(key)) ? CuckooIndex2(key) : CuckooIndex1(key);
                        }
                        count++;
                    }
                }
            }
        }
    }
    ASSERT(count == 3668);
    UNUSED(count);
}

bool SearchUtils::CanReachGameCycle(const NodeInfo& node)
{
    if (node.position.GetHalfMoveCount() < 3)
    {
        return false;
    }

    if (node.isNullMove || node.previousMove.IsCapture() || node.previousMove.IsPromotion())
    {
        return false;
    }

    const uint64_t originalKey = node.position.GetHash();
    const NodeInfo* currNode = &node - 1;
    ASSERT(currNode);

    for (;;)
    {
        const NodeInfo* parentNode = currNode - 1;

        // go up the tree, abort on any null move or capture
        if (currNode->height < 2) break;
        if (currNode->isNullMove || parentNode->isNullMove) break;
        if (currNode->previousMove.IsCapture() || currNode->previousMove.GetPiece() == Piece::Pawn) break;
        if (parentNode->previousMove.IsCapture() || currNode->previousMove.GetPiece() == Piece::Pawn) break;

        currNode = currNode - 2;

        const uint64_t moveKey = originalKey ^ currNode->position.GetHash();

        uint32_t index = CuckooTableSize;
        if (gCuckooTable[CuckooIndex1(moveKey)] == moveKey) index = CuckooIndex1(moveKey);
        else if (gCuckooTable[CuckooIndex2(moveKey)] == moveKey) index = CuckooIndex2(moveKey);

        if (index < CuckooTableSize)
        {
            ASSERT(node.position.GetSideToMove() != currNode->position.GetSideToMove());
            const PackedMove move = gCuckooMoves[index];
            if (!(Bitboard::GetBetween(move.FromSquare(), move.ToSquare()) & node.position.Occupied()))
            {
                const Bitboard occupied = node.position.GetCurrentSide().Occupied();
                if (occupied & (move.FromSquare().GetBitboard() | move.ToSquare().GetBitboard()))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

void SearchUtils::GetPvLine(const NodeInfo& rootNode, uint32_t maxLength, std::vector<Move>& outLine)
{
    outLine.clear();

    if (maxLength > 0)
    {
        Position iteratedPosition = rootNode.position;

        uint32_t i = 0;

        // reconstruct PV line using PV array
        for (; i < std::min<uint32_t>(maxLength, rootNode.pvLength); ++i)
        {
            ASSERT(rootNode.pvLine[i].IsValid());
            const Move move = iteratedPosition.MoveFromPacked(rootNode.pvLine[i]);

            ASSERT(move.IsValid());
            if (!move.IsValid()) break;
            if (!iteratedPosition.DoMove(move)) break;

            outLine.push_back(move);
        }
    }
}

bool SearchUtils::IsRepetition(const NodeInfo& node, const Game& game)
{
    const NodeInfo* prevNode = &node;

    for (uint32_t ply = 1; ; ++ply)
    {
        // don't need to check more moves if reached pawn push or capture,
        // because these moves are irreversible
        if (prevNode->previousMove.IsValid())
        {
            if (prevNode->previousMove.GetPiece() == Piece::Pawn || prevNode->previousMove.IsCapture())
            {
                return false;
            }
        }

        // reached end of the stack
        if (prevNode->height == 0)
            break;

        --prevNode;

        // only check every second previous node, because side to move must be the same
        if (ply % 2 == 0)
        {
            ASSERT(prevNode->position.GetSideToMove() == node.position.GetSideToMove());

            if (prevNode->position.GetHash() == node.position.GetHash())
            {
                if (prevNode->position == node.position)
                {
                    return true;
                }
            }
        }
    }

    return game.GetRepetitionCount(node.position) >= 2;
}
