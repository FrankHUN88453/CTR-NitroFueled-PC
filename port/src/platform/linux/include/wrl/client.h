#pragma once
// Linux: the part of Microsoft::WRL::ComPtr that ctr_pc uses, for the COM interfaces of DXVK native.
#include <windows.h>

#include <unknwn.h>

#include <memory>
#include <utility>

namespace Microsoft::WRL {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(T* p) : p_(p) {
        if (p_) p_->AddRef();
    }
    ComPtr(const ComPtr& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { Reset(); }
    ComPtr& operator=(const ComPtr& o) {
        if (o.p_) o.p_->AddRef();
        Reset();
        p_ = o.p_;
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != std::addressof(o)) {
            Reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) {
        Reset();
        return *this;
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    // the address for an output parameter: releases what it held, like WRL
    T** operator&() {
        Reset();
        return &p_;
    }
    T** GetAddressOf() { return &p_; }
    T* const* GetAddressOf() const { return &p_; }
    T** ReleaseAndGetAddressOf() {
        Reset();
        return &p_;
    }
    unsigned long Reset() {
        unsigned long n = 0;
        if (p_) n = p_->Release();
        p_ = nullptr;
        return n;
    }
    template <class U>
    HRESULT As(U** out) const {
        return p_ ? p_->QueryInterface(__uuidof(U), (void**)out) : E_POINTER;
    }
    T* Detach() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }
    void Attach(T* p) {
        Reset();
        p_ = p;
    }
    bool operator==(std::nullptr_t) const { return p_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

}  // namespace Microsoft::WRL

template <class T>
inline IID CtrIidOf(T**) {  // by value: __uuidof may give a temporary
    return __uuidof(T);
}
#undef IID_PPV_ARGS
#define IID_PPV_ARGS(pp) CtrIidOf(pp), reinterpret_cast<void**>(pp)
