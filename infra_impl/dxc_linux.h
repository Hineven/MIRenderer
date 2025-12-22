/*
 * Created: 2024/9/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#pragma once

#include <cstdint>
#include <cstring>

// 定义Windows类型的Linux等价物
typedef int HRESULT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef uint32_t UINT32;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef unsigned long DWORD;
typedef int BOOL;  // 添加BOOL类型定义
#define FAILED(hr) (hr != 0)
#define CP_UTF8 65001

// GUID定义
struct GUID {
    unsigned long  Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char  Data4[8];
    
    bool operator==(const GUID& other) const {
        return memcmp(this, &other, sizeof(GUID)) == 0;
    }
};

// 前向声明接口
struct IDxcBlob;
struct IDxcBlobEncoding;
struct IDxcLibrary;
struct IDxcCompiler;
struct IDxcOperationResult;

// 定义接口ID - 直接导出常量
extern const GUID IDxcBlob_UUID;
extern const GUID IDxcBlobEncoding_UUID;
extern const GUID IDxcLibrary_UUID;
extern const GUID IDxcCompiler_UUID;
extern const GUID IDxcOperationResult_UUID;

// 类ID定义
extern const GUID CLSID_DxcLibrary;
extern const GUID CLSID_DxcCompiler;

// 简化版的IID_PPV_ARGS宏，不使用__uuidof
#define IID_PPV_ARGS_HELPER(Type, Pointer) const_cast<GUID&>(Type##_UUID), reinterpret_cast<void**>(Pointer)
#define IID_PPV_ARGS(Pointer) IID_PPV_ARGS_HELPER(IDxc##Pointer, Pointer)

// 基础COM接口
struct IUnknown {
    virtual HRESULT QueryInterface(const GUID& riid, void** ppvObject) = 0;
    virtual UINT32 AddRef() = 0;
    virtual UINT32 Release() = 0;
    virtual ~IUnknown() {} // 添加虚析构函数避免警告
};

// DXC接口定义
struct IDxcBlob : public IUnknown {
    virtual LPCVOID GetBufferPointer() = 0;
    virtual size_t GetBufferSize() = 0;
};

struct IDxcBlobEncoding : public IDxcBlob {
    virtual HRESULT GetEncoding(BOOL *pKnown, UINT32 *pCodePage) = 0;
};

struct IDxcLibrary : public IUnknown {
    virtual HRESULT CreateBlobFromFile(const WCHAR *pFileName, UINT32 *codePage, IDxcBlobEncoding **pBlobEncoding) = 0;
    virtual HRESULT CreateBlobWithEncodingFromPinned(LPCVOID pText, UINT32 size, UINT32 codePage, IDxcBlobEncoding **pBlobEncoding) = 0;
    virtual HRESULT CreateBlobWithEncodingOnHeapCopy(LPCVOID pText, UINT32 size, UINT32 codePage, IDxcBlobEncoding **pBlobEncoding) = 0;
    virtual HRESULT CreateBlobWithEncodingOnMalloc(LPCVOID pText, UINT32 size, UINT32 codePage, void *pMalloc, IDxcBlobEncoding **pBlobEncoding) = 0;
    virtual HRESULT CreateIncludeHandler(void **ppResult) = 0;
    virtual HRESULT CreateStreamFromBlobReadOnly(IDxcBlob *pBlob, void **ppStream) = 0;
    virtual HRESULT CreateBlobFromStream(void *pStream, UINT32 *pCodePage, IDxcBlobEncoding **ppBlobEncoding) = 0;
};

struct IDxcCompiler : public IUnknown {
    virtual HRESULT Compile(
        IDxcBlob *pSource,
        const WCHAR *pSourceName,
        const WCHAR *pEntryPoint,
        const WCHAR *pTargetProfile,
        const WCHAR **pArguments,
        UINT32 argCount,
        const void **pDefines,
        UINT32 defineCount,
        void *pIncludeHandler,
        IDxcOperationResult **ppResult) = 0;
    
    virtual HRESULT Preprocess(
        IDxcBlob *pSource,
        const WCHAR *pSourceName,
        const WCHAR **pArguments,
        UINT32 argCount,
        const void **pDefines,
        UINT32 defineCount,
        void *pIncludeHandler,
        IDxcOperationResult **ppResult) = 0;
    
    virtual HRESULT Disassemble(IDxcBlob *pSource, IDxcBlobEncoding **ppDisassembly) = 0;
};

struct IDxcOperationResult : public IUnknown {
    virtual HRESULT GetStatus(HRESULT *pStatus) = 0;
    virtual HRESULT GetResult(IDxcBlob **ppResult) = 0;
    virtual HRESULT GetErrorBuffer(IDxcBlobEncoding **ppErrors) = 0;
};

// DXC创建实例函数类型
typedef HRESULT (*DxcCreateInstanceProc)(
    const GUID& rclsid,
    const GUID& riid,
    void** ppv);
