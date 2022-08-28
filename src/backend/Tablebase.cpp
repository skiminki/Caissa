#include "Tablebase.hpp"
#include "Position.hpp"
#include "Move.hpp"
#include "MoveList.hpp"

#ifdef USE_TABLE_BASES

#include "syzygy/tbprobe.h"
#include "gaviota/gtb-probe.h"

#include <iostream>
#include <mutex>

static std::mutex g_syzygyMutex;
static std::mutex g_gaviotaMutex;

static const uint32_t g_gaviotaWdlFraction = 32; // 25% for WDL information

static size_t g_gaviotaPendingCacheSize = 0;

void LoadSyzygyTablebase(const char* path)
{
    std::unique_lock lock(g_syzygyMutex);

    if (syzygy_tb_init(path))
    {
        std::cout << "info string Syzygy tablebase loaded successfully. Size = " << TB_LARGEST << std::endl;
    }
    else
    {
        std::cout << "info string Failed to load Syzygy tablebase" << std::endl;
    }
}

void LoadGaviotaTablebase(const char* path)
{
    std::unique_lock lock(g_gaviotaMutex);

    const int32_t verbosity = 0;

    const char** paths = tbpaths_init();
    paths = tbpaths_add(paths, path);

    const char* ret = tb_init(verbosity, tb_CP4, paths);

    if (!ret)
    {
        std::cout << "info string Gaviota tablebases loaded successfully. Availability = " << tb_availability() << std::endl;
    }
    else
    {
        std::cout << "info string Failed to load Gaviota tablebase: " << ret << std::endl;
    }

    if (g_gaviotaPendingCacheSize)
    {
        SetGaviotaCacheSize(g_gaviotaPendingCacheSize);
    }
}

void SetGaviotaCacheSize(size_t cacheSize)
{
    if (tb_availability() != 0)
    {
        tbcache_init(cacheSize, g_gaviotaWdlFraction);
        g_gaviotaPendingCacheSize = 0;
    }
    else
    {
        g_gaviotaPendingCacheSize = cacheSize;
    }
}

void UnloadTablebase()
{
    {
        std::unique_lock lock(g_syzygyMutex);
        tb_free();
    }

    {
        std::unique_lock lock(g_gaviotaMutex);
        tbcache_done();
        tb_done();
    }
}

bool HasSyzygyTablebases()
{
    return TB_LARGEST > 0u;
}

bool HasGaviotaTablebases()
{
    return tb_availability() != 0;
}

static Piece TranslatePieceType(uint32_t tbPromotes)
{
    switch (tbPromotes)
    {
    case TB_PROMOTES_QUEEN:     return Piece::Queen;
    case TB_PROMOTES_ROOK:      return Piece::Rook;
    case TB_PROMOTES_BISHOP:    return Piece::Bishop;
    case TB_PROMOTES_KNIGHT:    return Piece::Knight;
    }
    return Piece::None;
}

bool ProbeSyzygy_Root(const Position& pos, Move& outMove, uint32_t* outDistanceToZero, int32_t* outWDL)
{
    if (pos.GetNumPieces() > TB_LARGEST)
    {
        return false;
    }

    std::unique_lock lock(g_syzygyMutex);

    uint32_t castlingRights = 0;
    if (pos.GetWhitesCastlingRights() & CastlingRights_ShortCastleAllowed)  castlingRights |= TB_CASTLING_K;
    if (pos.GetWhitesCastlingRights() & CastlingRights_LongCastleAllowed)   castlingRights |= TB_CASTLING_Q;
    if (pos.GetBlacksCastlingRights() & CastlingRights_ShortCastleAllowed)  castlingRights |= TB_CASTLING_k;
    if (pos.GetBlacksCastlingRights() & CastlingRights_LongCastleAllowed)   castlingRights |= TB_CASTLING_q;

    const uint32_t probeResult = tb_probe_root(
        pos.Whites().Occupied(),
        pos.Blacks().Occupied(),
        pos.Whites().king | pos.Blacks().king,
        pos.Whites().queens | pos.Blacks().queens,
        pos.Whites().rooks | pos.Blacks().rooks,
        pos.Whites().bishops | pos.Blacks().bishops,
        pos.Whites().knights | pos.Blacks().knights,
        pos.Whites().pawns | pos.Blacks().pawns,
        pos.GetHalfMoveCount(),
        castlingRights,
        pos.GetEnPassantSquare().IsValid() ? pos.GetEnPassantSquare().Index() : 0,
        pos.GetSideToMove() == Color::White,
        nullptr);

    if (probeResult == TB_RESULT_FAILED)
    {
        return false;
    }

    const uint32_t tbFrom = TB_GET_FROM(probeResult);
    const uint32_t tbTo = TB_GET_TO(probeResult);
    const uint32_t tbPromo = TB_GET_PROMOTES(probeResult);

    outMove = pos.MoveFromPacked(PackedMove(Square(tbFrom), Square(tbTo), TranslatePieceType(tbPromo)));

    if (!outMove.IsValid())
    {
        return false;
    }

    if (outDistanceToZero)
    {
        *outDistanceToZero = TB_GET_DTZ(probeResult);
    }

    if (outWDL)
    {
        *outWDL = 0;

        if (TB_GET_WDL(probeResult) == TB_WIN) *outWDL = 1;
        if (TB_GET_WDL(probeResult) == TB_LOSS) *outWDL = -1;
    }

    return true;
}

bool ProbeSyzygy_WDL(const Position& pos, int32_t* outWDL)
{
    ASSERT(pos.IsValid());
    ASSERT(!pos.IsInCheck(GetOppositeColor(pos.GetSideToMove())));

    if (pos.GetNumPieces() > TB_LARGEST)
    {
        return false;
    }

    uint32_t castlingRights = 0;
    if (pos.GetWhitesCastlingRights() & CastlingRights_ShortCastleAllowed)  castlingRights |= TB_CASTLING_K;
    if (pos.GetWhitesCastlingRights() & CastlingRights_LongCastleAllowed)   castlingRights |= TB_CASTLING_Q;
    if (pos.GetBlacksCastlingRights() & CastlingRights_ShortCastleAllowed)  castlingRights |= TB_CASTLING_k;
    if (pos.GetBlacksCastlingRights() & CastlingRights_LongCastleAllowed)   castlingRights |= TB_CASTLING_q;

    // TODO skip if too many pieces, obvious wins, etc.
    const uint32_t probeResult = tb_probe_wdl(
        pos.Whites().Occupied(),
        pos.Blacks().Occupied(),
        pos.Whites().king | pos.Blacks().king,
        pos.Whites().queens | pos.Blacks().queens,
        pos.Whites().rooks | pos.Blacks().rooks,
        pos.Whites().bishops | pos.Blacks().bishops,
        pos.Whites().knights | pos.Blacks().knights,
        pos.Whites().pawns | pos.Blacks().pawns,
        castlingRights,
        pos.GetEnPassantSquare().IsValid() ? pos.GetEnPassantSquare().Index() : 0,
        pos.GetSideToMove() == Color::White);

    if (probeResult != TB_RESULT_FAILED)
    {
#ifdef COLLECT_SEARCH_STATS
        ctx.stats.tbHits++;
#endif // COLLECT_SEARCH_STATS

        // wins/losses are certain only if half move count is zero
        if (pos.GetHalfMoveCount() == 0)
        {
            if (probeResult == TB_LOSS)
            {
                *outWDL = -1;
                return true;
            }
            else if (probeResult == TB_WIN)
            {
                *outWDL = 1;
                return true;
            }
        }

        // draws are certain, no matter the half move counter
        if (probeResult != TB_LOSS && probeResult != TB_WIN)
        {
            *outWDL = 0;
            return true;
        }
    }

    return false;
}

static TB_squares SquareToGaviota(const Square square)
{
    return square.IsValid() ? (TB_squares)square.Index() : tb_NOSQUARE;
}

static TB_pieces PieceToGaviota(const Piece piece)
{
    static_assert((uint32_t)Piece::None == (uint32_t)tb_NOPIECE);
    static_assert((uint32_t)Piece::Pawn == (uint32_t)tb_PAWN);
    static_assert((uint32_t)Piece::Knight == (uint32_t)tb_KNIGHT);
    static_assert((uint32_t)Piece::Bishop == (uint32_t)tb_BISHOP);
    static_assert((uint32_t)Piece::Rook == (uint32_t)tb_ROOK);
    static_assert((uint32_t)Piece::Queen == (uint32_t)tb_QUEEN);
    static_assert((uint32_t)Piece::King == (uint32_t)tb_KING);
    return (TB_pieces)piece;
}

bool ProbeGaviota(const Position& pos, uint32_t* outDTM, int32_t* outWDL)
{
    if (tb_availability() == 0)
    {
        return false;
    }

    if (pos.GetNumPieces() > 5)
    {
        return false;
    }

    uint32_t pliesToMate = 0;
    uint32_t info = tb_UNKNOWN;

    uint32_t stm = pos.GetSideToMove() == Color::White ? tb_WHITE_TO_MOVE : tb_BLACK_TO_MOVE;
    uint32_t epsquare = SquareToGaviota(pos.GetEnPassantSquare());

    uint32_t castlingRights = tb_NOCASTLE;
    if (pos.GetWhitesCastlingRights() & CastlingRights_ShortCastleAllowed)  castlingRights |= tb_WOO;
    if (pos.GetWhitesCastlingRights() & CastlingRights_LongCastleAllowed)   castlingRights |= tb_WOOO;
    if (pos.GetBlacksCastlingRights() & CastlingRights_ShortCastleAllowed)  castlingRights |= tb_BOO;
    if (pos.GetBlacksCastlingRights() & CastlingRights_LongCastleAllowed)   castlingRights |= tb_BOOO;

    uint32_t    ws[17];     // list of squares for white
    uint32_t    bs[17];     // list of squares for black
    uint8_t     wp[17];     // what white pieces are on those squares
    uint8_t     bp[17];     // what black pieces are on those squares

    // write white pieces
    {
        uint32_t index = 0;

        ws[index] = FirstBitSet(pos.Whites().king);
        wp[index] = tb_KING;
        index++;

        pos.Whites().pawns.Iterate([&](uint32_t square) INLINE_LAMBDA{ ws[index] = square; wp[index] = tb_PAWN; index++; });
        pos.Whites().knights.Iterate([&](uint32_t square) INLINE_LAMBDA{ ws[index] = square; wp[index] = tb_KNIGHT; index++; });
        pos.Whites().bishops.Iterate([&](uint32_t square) INLINE_LAMBDA{ ws[index] = square; wp[index] = tb_BISHOP; index++; });
        pos.Whites().rooks.Iterate([&](uint32_t square) INLINE_LAMBDA{ ws[index] = square; wp[index] = tb_ROOK; index++; });
        pos.Whites().queens.Iterate([&](uint32_t square) INLINE_LAMBDA{ ws[index] = square; wp[index] = tb_QUEEN; index++; });

        wp[index] = tb_NOPIECE;
        ws[index] = tb_NOSQUARE;
    }

    // write black pieces
    {
        uint32_t index = 0;

        bs[index] = FirstBitSet(pos.Blacks().king);
        bp[index] = tb_KING;
        index++;

        pos.Blacks().pawns.Iterate([&](uint32_t square) INLINE_LAMBDA{ bs[index] = square; bp[index] = tb_PAWN; index++; });
        pos.Blacks().knights.Iterate([&](uint32_t square) INLINE_LAMBDA{ bs[index] = square; bp[index] = tb_KNIGHT; index++; });
        pos.Blacks().bishops.Iterate([&](uint32_t square) INLINE_LAMBDA{ bs[index] = square; bp[index] = tb_BISHOP; index++; });
        pos.Blacks().rooks.Iterate([&](uint32_t square) INLINE_LAMBDA{ bs[index] = square; bp[index] = tb_ROOK; index++; });
        pos.Blacks().queens.Iterate([&](uint32_t square) INLINE_LAMBDA{ bs[index] = square; bp[index] = tb_QUEEN; index++; });

        bp[index] = tb_NOPIECE;
        bs[index] = tb_NOSQUARE;
    }

    if (outDTM)
    {
        if (0 == tb_probe_hard(stm, epsquare, castlingRights, ws, bs, wp, bp, &info, &pliesToMate))
        {
            return false;
        }
    }
    else
    {
        if (0 == tb_probe_WDL_hard(stm, epsquare, castlingRights, ws, bs, wp, bp, &info))
        {
            return false;
        }
    }

    if (info == tb_DRAW)
    {
        if (outWDL) *outWDL = 0;
    }
    else if (info == tb_WMATE)
    {
        if (outWDL) *outWDL = 1;
    }
    else if (info == tb_BMATE)
    {
        if (outWDL) *outWDL = -1;
    }
    else
    {
        return false;
    }

    if (outDTM)
    {
        *outDTM = pliesToMate;
    }

    return true;
}

bool ProbeGaviota_Root(const Position& pos, Move& outMove, uint32_t* outDTM, int32_t* outWDL)
{
    if (!ProbeGaviota(pos, outDTM, outWDL))
    {
        return false;
    }

    MoveList moves;
    pos.GenerateMoveList(moves);

    if (moves.Size() == 0)
    {
        return 0;
    }

    Move bestMove = Move::Invalid();
    int32_t bestScore = -InfValue;

    for (uint32_t i = 0; i < moves.Size(); ++i)
    {
        const Move move = moves[i].move;
        ASSERT(move.IsValid());

        Position childPosition = pos;
        if (childPosition.DoMove(move))
        {
            int32_t wdl = 0;
            uint32_t dtm = 0;
            if (!ProbeGaviota(childPosition, &dtm, &wdl))
            {
                return false;
            }

            int32_t score = 0;
            if (wdl < 0) score = -CheckmateValue + dtm;
            if (wdl > 0) score =  CheckmateValue - dtm;

            if (pos.GetSideToMove() == Color::Black) score = -score;

            if (score > bestScore)
            {
                bestScore = score;
                bestMove = move;
            }
        }
    }

    outMove = bestMove;

    return bestScore > -InfValue;
}

#else // !USE_TABLE_BASES

bool HasTablebases()
{
    return false;
}

void LoadSyzygyTablebase(const char*) { }
void LoadGaviotaTablebase(const char*) { }
void SetGaviotaCacheSize(size_t) { }
void UnloadTablebase() { }

bool ProbeSyzygy_Root(const Position&, Move&, uint32_t*, int32_t*)
{
    return false;
}

bool ProbeSyzygy_WDL(const Position&, int32_t*)
{
    return false;
}

bool ProbeGaviota(const Position&, uint32_t*, int32_t*)
{
    return false;
}

#endif // USE_TABLE_BASES
