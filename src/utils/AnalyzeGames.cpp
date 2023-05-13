#include "Common.hpp"
#include "GameCollection.hpp"
#include "ThreadPool.hpp"

#include "../backend/Position.hpp"
#include "../backend/Material.hpp"
#include "../backend/Game.hpp"
#include "../backend/Move.hpp"
#include "../backend/Search.hpp"
#include "../backend/TranspositionTable.hpp"
#include "../backend/Evaluate.hpp"
#include "../backend/Endgame.hpp"
#include "../backend/Tablebase.hpp"
#include "../backend/Waitable.hpp"
#include "../backend/Time.hpp"

#include <filesystem>
#include <chrono>
#include <random>
#include <mutex>
#include <fstream>
#include <limits.h>
#include <iomanip>

struct MaterialConfigInfo
{
    uint64_t occurences = 0;
    double evalScore = 0.0;
    double gameScore = 0.0;
};

float GameScoreToExpectedGameScore(const Game::Score score)
{
    switch (score)
    {
    case Game::Score::BlackWins: return 0.0f;
    case Game::Score::WhiteWins: return 1.0f;
    default: return 0.5f;
    }
}

struct GamesStats
{
    std::mutex mutex;
    std::ofstream fortressPosition;

    uint64_t numGames = 0;
    uint64_t numPositions = 0;
    uint64_t numPawnlessPositions = 0;
};

void AnalyzeGames(const char* path, GamesStats& stats)
{
    FileInputStream gamesFile(path);

    uint64_t numGames = 0;
    uint64_t numPositions = 0;
    uint64_t numPawnlessPositions = 0;

    //std::unordered_map<MaterialKey, MaterialConfigInfo> materialConfigurations;
    
    uint64_t pieceOccupancy[6][64] = { };

    uint64_t gameResultVsHalfMoveCounter[3][101] = { };

    TimePoint startTime = TimePoint::GetCurrent();

    Game game;
    std::vector<Move> moves;

    while (GameCollection::ReadGame(gamesFile, game, moves))
    {
        Position pos = game.GetInitialPosition();

        ASSERT(game.GetMoves().size() == game.GetMoveScores().size());

        if (game.GetScore() == Game::Score::Unknown)
        {
            continue;
        }

        //std::cout << game.ToPGN(true);
        //std::cout << std::endl << std::endl;

        for (size_t i = 0; i < game.GetMoves().size(); ++i)
        {
            const Move move = pos.MoveFromPacked(game.GetMoves()[i]);
            const ScoreType moveScore = game.GetMoveScores()[i];

            if (!move.IsQuiet() &&
                std::abs(moveScore) < KnownWinValue &&
                !pos.IsInCheck(pos.GetSideToMove()))
            {
                const MaterialKey matKey = pos.GetMaterialKey();

                if (pos.GetHalfMoveCount() <= 100)
                {
                    gameResultVsHalfMoveCounter[(uint32_t)game.GetScore()][pos.GetHalfMoveCount()]++;
                }

                numPositions++;
                if (matKey.numWhitePawns == 0 && matKey.numBlackPawns == 0) numPawnlessPositions++;

                // piece occupancy
                for (uint32_t pieceIndex = 0; pieceIndex < 6; ++pieceIndex)
                {
                    const Piece piece = (Piece)(pieceIndex + (uint32_t)Piece::Pawn);
                    pos.Whites().GetPieceBitBoard(piece).Iterate([&](const uint32_t square) INLINE_LAMBDA {
                        pieceOccupancy[pieceIndex][square]++; });
                    pos.Blacks().GetPieceBitBoard(piece).Iterate([&](const uint32_t square) INLINE_LAMBDA {
                        pieceOccupancy[pieceIndex][Square(square).FlippedRank().Index()]++; });
                }

                //MaterialConfigInfo& matConfigInfo = materialConfigurations[matKey];
                //matConfigInfo.occurences++;
                //matConfigInfo.evalScore += CentiPawnToExpectedGameScore(moveScore);
                //matConfigInfo.gameScore += GameScoreToExpectedGameScore(game.GetScore());

                //const MaterialKey key = pos.GetMaterialKey();
                //if (key.numWhitePawns > 1 && key.numWhiteKnights == 0 && key.numWhiteBishops == 0 && key.numWhiteRooks == 0 && key.numWhiteQueens == 0 &&
                //    key.numBlackPawns > 1 && key.numBlackKnights == 0 && key.numBlackBishops == 0 && key.numBlackRooks == 0 && key.numBlackQueens == 0)
                //{
                //    std::cout << pos.ToFEN() << " score: " << game.GetMoveScores()[i] << std::endl;
                //    break;
                //}
            }

            // dump potential fortress positions
            {
                const int32_t fortressTreshold = 200;
                int32_t wdl = 0;

                if (move.IsQuiet() &&
                    pos.GetNumPieces() <= 7 && pos.GetNumPieces() >= 4 &&
                    pos.GetHalfMoveCount() > 20)
                {
                    const ScoreType eval = Evaluate(pos);
                    if ((eval > fortressTreshold && moveScore > fortressTreshold) ||
                        (eval < -fortressTreshold && moveScore < -fortressTreshold))
                    {
                        if (ProbeSyzygy_WDL(pos, &wdl) && wdl == 0)
                        {
                            std::unique_lock<std::mutex> lock(stats.mutex);
                            stats.fortressPosition << pos.ToFEN() << std::endl;
                            break;
                        }
                    }
                }
            }

            if (!pos.DoMove(move))
            {
                break;
            }
        }

        numGames++;
    }

    {
        std::unique_lock<std::mutex> lock(stats.mutex);

        std::cout << "Parsed " << numGames << " games" << std::endl;
        std::cout << "Found " << numPositions << " positions" << std::endl;
        std::cout << "Found " << numPawnlessPositions << " pawnless positions" << std::endl;

        stats.numGames += numGames;
        stats.numPositions += numPositions;
        stats.numPawnlessPositions += numPawnlessPositions;
    }

    /*
    {
        std::cout << "Unique material configurations: " << materialConfigurations.size() << std::endl;
        for (const auto& iter : materialConfigurations)
        {
            if (iter.second.occurences > 5 && iter.first.numBlackPawns == 0 && iter.first.numWhitePawns == 0)
            {
                const float averageEvalScore = static_cast<float>(iter.second.evalScore / static_cast<double>(iter.second.occurences));
                const float averageGameScore = static_cast<float>(iter.second.gameScore / static_cast<double>(iter.second.occurences));

                std::cout
                    << std::setw(33) << iter.first.ToString() << " "
                    << std::showpos << std::fixed << std::setprecision(2) << ExpectedGameScoreToPawns(averageEvalScore) << " "
                    << std::showpos << std::fixed << std::setprecision(2) << ExpectedGameScoreToPawns(averageGameScore) << std::endl;
                std::cout << std::resetiosflags(std::ios_base::showpos);
            }
        }
        std::cout << std::endl;
    }

    {
        std::cout << "WDL vs. half-move counter: " << std::endl;
        for (uint32_t ply = 0; ply <= 100; ++ply)
        {
            std::cout
                << std::setw(5) << ply << " "
                << std::setw(10) << gameResultVsHalfMoveCounter[0][ply] << " "
                << std::setw(10) << gameResultVsHalfMoveCounter[1][ply] << " "
                << std::setw(10) << gameResultVsHalfMoveCounter[2][ply] << std::endl;
        }
    }

    for (uint32_t pieceIndex = 0; pieceIndex < 6; ++pieceIndex)
    {
        const Piece piece = (Piece)(pieceIndex + (uint32_t)Piece::Pawn);
        std::cout << PieceToString(piece) << " occupancy: " << std::endl;
        for (uint32_t rank = 0; rank < 8; ++rank)
        {
            for (uint32_t file = 0; file < 8; ++file)
            {
                std::cout << std::setw(10) << pieceOccupancy[pieceIndex][8 * rank + file] << " ";
            }
            std::cout << std::endl;
        }
        std::cout << std::endl;
    }
    */
}

void AnalyzeGames()
{
    GamesStats stats;
    stats.fortressPosition.open("fortress.epd");

    const std::string gamesPath = DATA_PATH "selfplayGames/";

    Waitable waitable;
    {
        threadpool::TaskBuilder taskBuilder(waitable);

        for (const auto& path : std::filesystem::directory_iterator(gamesPath))
        {
            std::cout << "Loading " << path.path().string() << "..." << std::endl;

            taskBuilder.Task("LoadPositions", [path, &stats](const threadpool::TaskContext&)
            {
                AnalyzeGames(path.path().string().c_str(), stats);
            });
        }
    }

    waitable.Wait();
}
