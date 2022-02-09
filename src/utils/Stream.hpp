#pragma once

#include "../backend/Common.hpp"

#include <vector>

class InputStream
{
public:
    ~InputStream() = default;
    virtual uint64_t GetSize() = 0;
    virtual bool Read(void* data, size_t size) = 0;
};

class OutputStream
{
public:
    ~OutputStream() = default;
    virtual uint64_t GetSize() = 0;
    virtual bool Write(const void* data, size_t size) = 0;
};

//////////////////////////////////////////////////////////////////////////

class MemoryInputStream : public InputStream
{
public:
    MemoryInputStream(const std::vector<uint8_t>& buffer);
    virtual uint64_t GetSize() override;
    virtual bool Read(void* data, size_t size) override;
private:
    const std::vector<uint8_t>& mBuffer;
    size_t mPosition;
};

class MemoryOutputStream : public OutputStream
{
public:
    MemoryOutputStream(std::vector<uint8_t>& buffer);
    virtual uint64_t GetSize() override;
    virtual bool Write(const void* data, size_t size) override;
private:
    std::vector<uint8_t>& mBuffer;
};

//////////////////////////////////////////////////////////////////////////

class FileInputStream : public InputStream
{
public:
    FileInputStream(const char* filePath);
    bool IsOpen() const;
    virtual uint64_t GetSize() override;
    virtual bool Read(void* data, size_t size) override;
private:
    FILE* mFile;
};

class FileOutputStream : public OutputStream
{
public:
    FileOutputStream(const char* filePath);
    bool IsOpen() const;
    bool Seek(uint64_t pos);
    virtual uint64_t GetSize() override;
    virtual bool Write(const void* data, size_t size) override;
private:
    FILE* mFile;
};