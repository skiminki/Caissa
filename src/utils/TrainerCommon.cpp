#include "Common.hpp"
#include "ThreadPool.hpp"
#include "TrainerCommon.hpp"

#include "../backend/Math.hpp"
#include "../backend/Evaluate.hpp"
#include "../backend/NeuralNetworkEvaluator.hpp"

#include <filesystem>

static_assert(sizeof(PositionEntry) == 32, "Invalid PositionEntry size");

bool TrainingDataLoader::Init(std::mt19937& gen, const std::string& trainingDataPath)
{
    uint64_t totalDataSize = 0;

    mCDF.push_back(0.0);

    for (const auto& path : std::filesystem::directory_iterator(trainingDataPath))
    {
        const std::string& fileName = path.path().string();
        auto fileStream = std::make_unique<FileInputStream>(fileName.c_str());

        uint64_t fileSize = fileStream->GetSize();
        totalDataSize += fileSize;

        if (fileStream->IsOpen() && fileSize > sizeof(PositionEntry))
        {
            std::cout << "Using " << fileName << std::endl;

            InputFileContext& ctx = mContexts.emplace_back();
            ctx.fileStream = std::move(fileStream);
            ctx.fileName = fileName;
            ctx.fileSize = fileSize;

            // Seek to random location so that each stream starts at different position.
            {
                const uint64_t numEntries = fileSize / sizeof(PositionEntry);
                std::uniform_int_distribution<uint64_t> distr(0, numEntries - 1);
                const uint64_t entryIndex = distr(gen);
                ctx.fileStream->SetPosition(entryIndex * sizeof(PositionEntry));
            }

            // Set a small, random skipping probability.
            // The idea is to have each stream running at different rates
            // so there's lower chance of generating similar batches from different streams.
            // Basically, it's another layer of data shuffling.
            {
                std::uniform_real_distribution<float> distr(0.0f, 0.1f);
                ctx.skippingProbability = distr(gen);
            }

            mCDF.push_back((double)totalDataSize);
        }
        else
        {
            std::cout << "ERROR: Failed to load selfplay data file: " << fileName << std::endl;
        }
    }

    if (totalDataSize > 0)
    {
        // normalize
        for (double& v : mCDF)
        {
            v /= static_cast<double>(totalDataSize);
        }
    }

    return !mContexts.empty();
}

uint32_t TrainingDataLoader::SampleInputFileIndex(double u) const
{
    uint32_t low = 0u;
    uint32_t high = static_cast<uint32_t>(mContexts.size());

    // binary search
    while (low < high)
    {
        uint32_t mid = (low + high) / 2u;
        if (u >= mCDF[mid])
        {
            low = mid + 1u;
        }
        else
        {
            high = mid;
        }
    }

    return low - 1u;
}

bool TrainingDataLoader::FetchNextPosition(std::mt19937& gen, PositionEntry& outEntry, Position& outPosition, uint64_t kingBucketMask)
{
    std::uniform_real_distribution<double> distr;
    const double u = distr(gen);
    const uint32_t fileIndex = SampleInputFileIndex(u);
    ASSERT(fileIndex < mContexts.size());

    if (fileIndex >= mContexts.size())
        return false;

    return mContexts[fileIndex].FetchNextPosition(gen, outEntry, outPosition, kingBucketMask);
}

bool TrainingDataLoader::InputFileContext::FetchNextPosition(std::mt19937& gen, PositionEntry& outEntry, Position& outPosition, uint64_t kingBucketMask)
{
    for (;;)
    {
        if (!fileStream->Read(&outEntry, sizeof(PositionEntry)))
        {
            // if read failed, reset to the file beginning and try again

            if (fileStream->GetPosition() > 0)
            {
                std::cout << "Resetting stream " << fileName << std::endl;
                fileStream->SetPosition(0);
            }
            else
            {
                return false;
            }

            if (!fileStream->Read(&outEntry, sizeof(PositionEntry)))
            {
                return false;
            }
        }

        // skip invalid scores
        if (outEntry.score >= CheckmateValue || outEntry.score <= -CheckmateValue)
            continue;

        // constant skipping
        {
            std::bernoulli_distribution skippingDistr(skippingProbability);
            if (skippingDistr(gen))
                continue;
        }

        if (kingBucketMask != UINT64_MAX)
        {
            // skip drawn game based half-move counter
            if (outEntry.wdlScore == (uint8_t)Game::Score::Draw)
            {
                const float hmcSkipProb = (float)outEntry.pos.halfMoveCount / 120.0f;
                std::bernoulli_distribution skippingDistr(hmcSkipProb);
                if (skippingDistr(gen))
                    continue;
            }

            // skip early moves
            constexpr uint32_t maxEarlyMoveCount = 12;
            if (outEntry.pos.moveCount < maxEarlyMoveCount)
            {
                const float earlyMoveSkipProb = 0.95f * (float)(maxEarlyMoveCount - outEntry.pos.moveCount - 1) / (float)maxEarlyMoveCount;
                std::bernoulli_distribution skippingDistr(earlyMoveSkipProb);
                if (skippingDistr(gen))
                    continue;
            }

            // skip based on piece count
            {
                const int32_t numPieces = outEntry.pos.occupied.Count();

                if (numPieces <= 3)
                    continue;

                if (numPieces <= 4 && std::bernoulli_distribution(0.9f)(gen))
                    continue;

                const float pieceCountSkipProb = Sqr(static_cast<float>(numPieces - 28) / 40.0f);
                if (pieceCountSkipProb > 0.0f && std::bernoulli_distribution(pieceCountSkipProb)(gen))
                    continue;
            }
        }

        VERIFY(UnpackPosition(outEntry.pos, outPosition, false));
        ASSERT(outPosition.IsValid());

        // filter by king bucket
        if (kingBucketMask != UINT64_MAX)
        {
            uint32_t whiteKingSide, blackKingSide;
            uint32_t whiteKingBucket, blackKingBucket;
            GetKingSideAndBucket(outPosition.Whites().GetKingSquare(), whiteKingSide, whiteKingBucket);
            GetKingSideAndBucket(outPosition.Blacks().GetKingSquare().FlippedRank(), blackKingSide, blackKingBucket);

            if ((((1ull << whiteKingBucket) & kingBucketMask) == 0ull) && (((1ull << blackKingBucket) & kingBucketMask) == 0ull))
                continue;
        }
        else
        {
            // skip based on kings placement (prefer king on further ranks)
            {
                const float whiteKingProb = 1.0f - (float)outPosition.Whites().GetKingSquare().Rank() / 7.0f;
                const float blackKingProb = (float)outPosition.Blacks().GetKingSquare().Rank() / 7.0f;
                std::bernoulli_distribution skippingDistr(0.25f * Sqr(std::min(whiteKingProb, blackKingProb)));
                if (skippingDistr(gen))
                    continue;
            }

            // skip based on WDL
            // the idea is to skip positions where for instance eval is high, but game result is loss
            {
                const uint32_t ply = 2 * outEntry.pos.moveCount;
                const float w = EvalToWinProbability(outEntry.score / 100.0f, ply);
                const float l = EvalToWinProbability(-outEntry.score / 100.0f, ply);
                const float d = 1.0f - w - l;

                float prob = d;
                if (outEntry.wdlScore == (uint8_t)Game::Score::WhiteWins) prob = w;
                if (outEntry.wdlScore == (uint8_t)Game::Score::BlackWins) prob = l;

                const float maxSkippingProb = 0.25f;
                std::bernoulli_distribution skippingDistr(maxSkippingProb * (1.0f - prob));
                if (skippingDistr(gen))
                    continue;
            }
        }

        return true;
    }
}
