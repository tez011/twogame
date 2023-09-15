#include "fs.h"
#include <SDL.h>

namespace twogame::fs {

class physfs_streambuf : public std::streambuf {
private:
    constexpr static size_t BUFFER_SIZE = 4096;
    char* m_buffer;

protected:
    PHYSFS_File* const m_fh;
    size_t m_fsz;

private:
    int_type underflow()
    {
        if (PHYSFS_eof(m_fh))
            return traits_type::eof();

        size_t consumed = PHYSFS_readBytes(m_fh, m_buffer, BUFFER_SIZE);
        if (consumed < 1)
            return traits_type::eof();
        setg(m_buffer, m_buffer, m_buffer + consumed);
        return static_cast<unsigned char>(*gptr());
    }

    pos_type seekoff(off_type pos, std::ios_base::seekdir dir, std::ios_base::openmode mode)
    {
        switch (dir) {
        case std::ios_base::beg:
            PHYSFS_seek(m_fh, pos);
            break;
        case std::ios_base::cur:
            PHYSFS_seek(m_fh, (PHYSFS_tell(m_fh) + pos) - (egptr() - gptr()));
            break;
        case std::ios_base::end:
            PHYSFS_seek(m_fh, m_fsz + pos);
            break;
        default:
            std::terminate();
        }
        if (mode & std::ios_base::in) {
            setg(egptr(), egptr(), egptr());
        }
        if (mode & std::ios_base::out) {
            setp(m_buffer, m_buffer);
        }
        return PHYSFS_tell(m_fh);
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode mode)
    {
        PHYSFS_seek(m_fh, pos);
        if (mode & std::ios_base::in) {
            setg(egptr(), egptr(), egptr());
        }
        if (mode & std::ios_base::out) {
            setp(m_buffer, m_buffer);
        }
        return PHYSFS_tell(m_fh);
    }

    int_type overflow(int_type c = traits_type::eof())
    {
        if (pptr() == pbase() && c == traits_type::eof())
            return 0;
        if (PHYSFS_writeBytes(m_fh, pbase(), pptr() - pbase()) < 1)
            return traits_type::eof();
        if (c != traits_type::eof()) {
            if (PHYSFS_writeBytes(m_fh, &c, 1) < 1)
                return traits_type::eof();
        }
        return 0;
    }

    int sync()
    {
        return overflow();
    }

public:
    physfs_streambuf(PHYSFS_File* const fh, size_t fsz)
        : m_fh(fh)
        , m_fsz(fsz)
    {
        m_buffer = new char[BUFFER_SIZE];
        char* end = m_buffer + BUFFER_SIZE;
        setg(end, end, end);
        setp(m_buffer, end);
    }

    ~physfs_streambuf()
    {
        sync();
        delete[] m_buffer;
    }
};

BaseStream::BaseStream(PHYSFS_File* fh)
    : m_fh(fh)
{
}

BaseStream::~BaseStream()
{
    PHYSFS_close(m_fh);
}

InputStream::InputStream(const std::string& path)
    : BaseStream(PHYSFS_openRead(path.c_str()))
    , std::istream(new physfs_streambuf(m_fh, PHYSFS_fileLength(m_fh)))
{
}

InputStream::~InputStream()
{
    delete rdbuf();
}

OutputStream::OutputStream(const std::string& path, bool append)
    : BaseStream((append ? PHYSFS_openAppend : PHYSFS_openWrite)(path.c_str()))
    , std::ostream(new physfs_streambuf(m_fh, append ? PHYSFS_fileLength(m_fh) : 0))
{
}

OutputStream::~OutputStream()
{
    delete rdbuf();
}

}