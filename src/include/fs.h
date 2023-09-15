#pragma once
#include <iostream>
#include <list>
#include <physfs.h>
#include <string>
#include <vector>

namespace twogame::fs {

class BaseStream {
protected:
    PHYSFS_File* const m_fh;

public:
    BaseStream(PHYSFS_File*);
    virtual ~BaseStream();
};

class InputStream : public BaseStream, public std::istream {
private:
public:
    InputStream(const std::string&);
    virtual ~InputStream();
};

class OutputStream : public BaseStream, public std::ostream {
private:
public:
    OutputStream(const std::string&, bool append = false);
    virtual ~OutputStream();
};

}