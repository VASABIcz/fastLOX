#pragma once

template<typename T>
class VirtualCopy {
public:
    virtual T* deepCopy() const = 0;
};

#define VIRTUAL_COPY(name) public: name* deepCopy() const override { return new name(*this); } private:
#define PUB_VIRTUAL_COPY(name) name* deepCopy() const override { return new name(*this); }