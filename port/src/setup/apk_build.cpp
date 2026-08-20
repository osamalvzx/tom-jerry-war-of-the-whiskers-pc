// See apk_build.h for what this is and why v1 signing is the right choice here.
//
// THE GAME PACK. Every game file goes into ONE uncompressed zip entry, assets/game.pak, which
// the app unpacks on first launch (native_main.cpp). Two reasons it is a pack and not loose
// assets: AAssetManager cannot reliably enumerate directories, so the app would need an index
// anyway; and a single STORED entry keeps THIS file to byte-copying plus SHA-256, with no
// deflate implementation anywhere. Game textures and audio barely compress, so it costs
// nothing worth having.
//
//   "TJPK", u32 version=1, u32 fileCount,
//   fileCount x { u16 pathLen, path ('/'-separated, relative), u64 size },
//   then every file's bytes back to back, in the same order.
//
// ⚠ EVERY TEMPLATE ENTRY MUST BE STORED. A v1 signature digests each entry's UNCOMPRESSED
// bytes; storing them means the bytes we copy are the bytes we hash. port/tools/build_apk.ps1
// -Template guarantees it (apk_store_all.py re-packs even AndroidManifest.xml, which aapt2
// always deflates), and ReadTemplate below REFUSES anything deflated rather than writing an
// apk whose signature would silently not verify.
#include "setup/apk_build.h"

#include <windows.h>
#include <wincrypt.h>
#include <vector>
#include <string>
#include <objbase.h>   // CoCreateGuid
#include <string>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace tj::setup {
namespace {

// The PFX is a local cache of a locally generated key; the password only stops the
// file being usable by accident, and is not protecting anything from anyone.
static const wchar_t* const kPfxPassword = L"tjwow";

// ---------------------------------------------------------------- little-endian writers
void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (i * 8))); }
uint16_t Get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// ---------------------------------------------------------------- CRC32 (zip polynomial)
struct Crc32 {
    static const uint32_t* Table() {
        static uint32_t t[256];
        static bool built = false;
        if (!built) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            built = true;
        }
        return t;
    }
    uint32_t v = 0xFFFFFFFFu;
    void Update(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        const uint32_t* t = Table();
        for (size_t i = 0; i < n; ++i) v = t[(v ^ p[i]) & 0xFF] ^ (v >> 8);
    }
    uint32_t Final() const { return v ^ 0xFFFFFFFFu; }
};

// ---------------------------------------------------------------- SHA-256 + base64
struct Sha256 {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool Begin() {
        if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
        return CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash) != 0;
    }
    void Update(const void* d, size_t n) { if (hash && n) CryptHashData(hash, (const BYTE*)d, (DWORD)n, 0); }
    bool Final(std::vector<uint8_t>& out) {
        DWORD len = 32;
        out.resize(32);
        bool ok = hash && CryptGetHashParam(hash, HP_HASHVAL, out.data(), &len, 0);
        if (hash) CryptDestroyHash(hash);
        if (prov) CryptReleaseContext(prov, 0);
        hash = 0; prov = 0;
        return ok;
    }
    ~Sha256() { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); }
};

bool Sha256Of(const void* data, size_t n, std::vector<uint8_t>& out) {
    Sha256 h;
    if (!h.Begin()) return false;
    h.Update(data, n);
    return h.Final(out);
}

std::string Base64(const std::vector<uint8_t>& raw) {
    DWORD n = 0;
    if (!CryptBinaryToStringA(raw.data(), (DWORD)raw.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n)) return "";
    std::string s(n, '\0');
    if (!CryptBinaryToStringA(raw.data(), (DWORD)raw.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &s[0], &n)) return "";
    s.resize(strlen(s.c_str()));
    return s;
}

// ---------------------------------------------------------------- a file being written
struct OutFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t pos = 0;
    bool Open(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h != INVALID_HANDLE_VALUE;
    }
    bool Write(const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d;
        while (n) {
            DWORD put = 0;
            DWORD chunk = (DWORD)(n > (1u << 24) ? (1u << 24) : n);
            if (!WriteFile(h, p, chunk, &put, nullptr) || put == 0) return false;
            p += put; n -= put; pos += put;
        }
        return true;
    }
    bool Write(const std::vector<uint8_t>& v) { return v.empty() || Write(v.data(), v.size()); }
    // Patch a field already written (the pack's CRC, once its bytes are known).
    bool PatchAt(uint64_t at, const void* d, size_t n) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)at;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        DWORD put = 0;
        bool ok = WriteFile(h, d, (DWORD)n, &put, nullptr) && put == n;
        li.QuadPart = (LONGLONG)pos;
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        return ok;
    }
    void Close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    ~OutFile() { Close(); }
};

// ---------------------------------------------------------------- zip pieces
struct OutEntry {                 // what the central directory will need
    std::string name;
    uint32_t crc = 0;
    uint64_t size = 0;            // STORED: compressed == uncompressed
    uint64_t localOfs = 0;
};

void LocalHeader(std::vector<uint8_t>& v, const std::string& name, uint32_t crc, uint64_t size) {
    Put32(v, 0x04034b50);
    Put16(v, 20);                 // version needed
    Put16(v, 0);                  // flags — no data descriptor: sizes are known up front
    Put16(v, 0);                  // method 0 = STORED
    Put16(v, 0); Put16(v, 0);     // time, date
    Put32(v, crc);
    Put32(v, (uint32_t)size);
    Put32(v, (uint32_t)size);
    Put16(v, (uint16_t)name.size());
    Put16(v, 0);                  // extra len
    v.insert(v.end(), name.begin(), name.end());
}

// A STORED entry whose bytes are already in memory.
bool AddEntry(OutFile& out, std::vector<OutEntry>& dir, const std::string& name,
              const void* data, size_t n) {
    Crc32 c; c.Update(data, n);
    OutEntry e; e.name = name; e.crc = c.Final(); e.size = n; e.localOfs = out.pos;
    std::vector<uint8_t> hdr;
    LocalHeader(hdr, name, e.crc, n);
    if (!out.Write(hdr) || !out.Write(data, n)) return false;
    dir.push_back(e);
    return true;
}

bool WriteCentralDirectory(OutFile& out, const std::vector<OutEntry>& dir) {
    const uint64_t start = out.pos;
    std::vector<uint8_t> cd;
    for (const OutEntry& e : dir) {
        Put32(cd, 0x02014b50);
        Put16(cd, 20); Put16(cd, 20);
        Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put16(cd, 0);
        Put32(cd, e.crc);
        Put32(cd, (uint32_t)e.size);
        Put32(cd, (uint32_t)e.size);
        Put16(cd, (uint16_t)e.name.size());
        Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put16(cd, 0);
        Put32(cd, 0);
        Put32(cd, (uint32_t)e.localOfs);
        cd.insert(cd.end(), e.name.begin(), e.name.end());
    }
    if (!out.Write(cd)) return false;
    std::vector<uint8_t> eocd;
    Put32(eocd, 0x06054b50);
    Put16(eocd, 0); Put16(eocd, 0);
    Put16(eocd, (uint16_t)dir.size());
    Put16(eocd, (uint16_t)dir.size());
    Put32(eocd, (uint32_t)cd.size());
    Put32(eocd, (uint32_t)start);
    Put16(eocd, 0);
    return out.Write(eocd);
}

// ---------------------------------------------------------------- the template
struct TplEntry {
    std::string name;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

bool ReadTemplate(const uint8_t* apk, size_t n, std::vector<TplEntry>& out, std::string& err) {
    if (n < 22) { err = "The bundled Android template is corrupt."; return false; }
    size_t eocd = 0;
    bool found = false;
    size_t back = n > 66000 ? n - 66000 : 0;
    for (size_t i = n - 22; i + 1 > back; --i) {
        if (Get32(apk + i) == 0x06054b50) { eocd = i; found = true; break; }
        if (i == 0) break;
    }
    if (!found) { err = "The bundled Android template is not a valid archive."; return false; }
    const uint16_t count = Get16(apk + eocd + 10);
    const uint32_t cdOfs = Get32(apk + eocd + 16);
    size_t p = cdOfs;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > n || Get32(apk + p) != 0x02014b50) { err = "Android template: bad directory."; return false; }
        const uint16_t method = Get16(apk + p + 10);
        const uint32_t csize = Get32(apk + p + 20);
        const uint16_t nameLen = Get16(apk + p + 28);
        const uint16_t extraLen = Get16(apk + p + 30);
        const uint16_t cmtLen = Get16(apk + p + 32);
        const uint32_t lofs = Get32(apk + p + 42);
        std::string name((const char*)(apk + p + 46), nameLen);
        p += 46 + nameLen + extraLen + cmtLen;
        // Signature files are re-made here; skip whatever the template happened to carry.
        if (name.rfind("META-INF/", 0) == 0) continue;
        if (name.empty() || name.back() == '/') continue;         // directory record
        if (method != 0) {
            err = "The bundled Android template has a compressed entry (" + name +
                  "); it must be built with build_apk.ps1 -Template.";
            return false;
        }
        if (lofs + 30 > n) { err = "Android template: bad entry offset."; return false; }
        const uint16_t lNameLen = Get16(apk + lofs + 26);
        const uint16_t lExtraLen = Get16(apk + lofs + 28);
        const size_t dataAt = lofs + 30 + lNameLen + lExtraLen;
        if (dataAt + csize > n) { err = "Android template: entry runs past the file."; return false; }
        TplEntry e; e.name = name; e.data = apk + dataAt; e.size = csize;
        out.push_back(e);
    }
    return !out.empty();
}

// ---------------------------------------------------------------- the asset walk
struct PakFile {
    std::wstring full;
    std::string rel;              // '/'-separated, as the app will recreate it
    uint64_t size = 0;
};

void WalkDir(const std::wstring& dir, const std::string& relPrefix, std::vector<PakFile>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        char nameA[MAX_PATH];
        int an = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameA, sizeof nameA, nullptr, nullptr);
        if (an <= 0) continue;
        std::string rel = relPrefix.empty() ? std::string(nameA) : relPrefix + "/" + nameA;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WalkDir(full, rel, out);
        } else {
            PakFile f;
            f.full = full;
            f.rel = rel;
            f.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out.push_back(f);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- v1 (JAR) signing
std::string ManifestSection(const std::string& name, const std::string& digestB64) {
    // 72-byte line wrapping, per the jar spec: a continuation begins with a single space.
    std::string head = "Name: " + name;
    std::string wrapped;
    size_t at = 0;
    while (head.size() - at > 72) {
        wrapped += head.substr(at, 72) + "\r\n ";
        at += 72;
    }
    wrapped += head.substr(at);
    return wrapped + "\r\nSHA-256-Digest: " + digestB64 + "\r\n\r\n";
}

bool Pkcs7Sign(const void* pfx, size_t pfxBytes, const wchar_t* pw,
               const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& err) {
    CRYPT_DATA_BLOB blob;
    blob.pbData = (BYTE*)const_cast<void*>(pfx);
    blob.cbData = (DWORD)pfxBytes;
    // ⚠ NOT PKCS12_NO_PERSIST_KEY. That imports the private key as an EPHEMERAL CNG key, and
    // CryptSignMessage (a legacy CAPI entry point) then cannot reach it — signing fails with
    // NTE_BAD_KEYSET while every other step reports success. Importing to a user key set gives
    // CryptSignMessage a key it can use; CRYPT_USER_KEYSET keeps it out of the machine store.
    HCERTSTORE store = PFXImportCertStore(&blob, pw, CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
    if (!store) {
        err = "The signing key could not be read (error " + std::to_string((unsigned)GetLastError()) + ").";
        return false;
    }
    PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                     0, CERT_FIND_ANY, nullptr, nullptr);
    if (!cert) { CertCloseStore(store, 0); err = "The signing certificate is missing."; return false; }

    CRYPT_SIGN_MESSAGE_PARA para = {};
    para.cbSize = sizeof para;
    para.dwMsgEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    para.pSigningCert = cert;
    para.HashAlgorithm.pszObjId = const_cast<char*>(szOID_NIST_sha256);
    para.cMsgCert = 1;
    para.rgpMsgCert = &cert;

    const BYTE* msg = data.data();
    DWORD msgLen = (DWORD)data.size();
    DWORD outLen = 0;
    BOOL ok = CryptSignMessage(&para, TRUE /*detached*/, 1, &msg, &msgLen, nullptr, &outLen);
    if (ok) {
        out.resize(outLen);
        ok = CryptSignMessage(&para, TRUE, 1, &msg, &msgLen, out.data(), &outLen);
        out.resize(outLen);
    }
    if (!ok) err = "The Android package could not be signed (error " +
                   std::to_string((unsigned)GetLastError()) + ")."; 
    // The import above put the key in the user's key set; take it back out. The PFX file is
    // the only copy that should outlive this call.
    DWORD kpiLen = 0;
    if (CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &kpiLen) && kpiLen) {
        std::vector<uint8_t> kpiBuf(kpiLen);
        if (CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, kpiBuf.data(), &kpiLen)) {
            const CRYPT_KEY_PROV_INFO* kpi = (const CRYPT_KEY_PROV_INFO*)kpiBuf.data();
            HCRYPTPROV scrub = 0;
            CryptAcquireContextW(&scrub, kpi->pwszContainerName, kpi->pwszProvName,
                                 kpi->dwProvType, CRYPT_DELETEKEYSET);
        }
    }
    CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);
    return ok != FALSE;
}


// ---------------------------------------------------------------- the player's signing key
//
// Generated once, then reused forever. Android identifies an app by its signing certificate,
// so reusing this key is what lets a LATER installer build an apk that updates the player's
// existing install instead of being refused with INSTALL_FAILED_UPDATE_INCOMPATIBLE.
//
// It is deliberately NOT shipped with the installer. A key embedded in a published .exe is a
// published key -- anyone can extract it and sign an apk Android would treat as this same app.
// Generating per player costs nothing and avoids that entirely.
bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (16 << 20)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(h);
    return ok;
}

bool GenerateSigningKey(std::vector<uint8_t>& pfx, std::string& err) {
    // A named container is needed for CertCreateSelfSignCertificate to find the private key;
    // it is deleted again at the end, because the PFX bytes are the only copy we keep.
    wchar_t container[64];
    GUID g;
    CoCreateGuid(&g);
    swprintf_s(container, L"tjwow-apk-%08lx%04hx%04hx", g.Data1, g.Data2, g.Data3);

    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, container, nullptr, PROV_RSA_AES, CRYPT_NEWKEYSET)) {
        err = "A signing key could not be created."; return false;
    }
    HCRYPTKEY key = 0;
    if (!CryptGenKey(prov, AT_SIGNATURE, (2048 << 16) | CRYPT_EXPORTABLE, &key)) {
        CryptReleaseContext(prov, 0);
        CryptAcquireContextW(&prov, container, nullptr, PROV_RSA_AES, CRYPT_DELETEKEYSET);
        err = "A signing key could not be generated."; return false;
    }
    CryptDestroyKey(key);

    bool ok = false;
    BYTE nameBuf[512];
    DWORD nameLen = sizeof nameBuf;
    CERT_NAME_BLOB subject = { 0, nameBuf };
    if (CertStrToNameW(X509_ASN_ENCODING,
                       L"CN=Tom and Jerry War of the Whiskers, O=Native port",
                       CERT_X500_NAME_STR, nullptr, nameBuf, &nameLen, nullptr)) {
        subject.cbData = nameLen;
        CRYPT_KEY_PROV_INFO kpi = {};
        kpi.pwszContainerName = container;
        kpi.dwProvType = PROV_RSA_AES;
        kpi.dwKeySpec = AT_SIGNATURE;
        CRYPT_ALGORITHM_IDENTIFIER alg = {};
        alg.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);
        // Android wants a certificate that outlives the app; 30 years is the usual choice.
        SYSTEMTIME endSt;
        GetSystemTime(&endSt);
        endSt.wYear = (WORD)(endSt.wYear + 30);
        if (endSt.wMonth == 2 && endSt.wDay == 29) endSt.wDay = 28;   // leap-day safety
        PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(prov, &subject, 0, &kpi, &alg,
                                                            nullptr, &endSt, nullptr);
        if (cert) {
            HCERTSTORE mem = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
            if (mem && CertAddCertificateContextToStore(mem, cert, CERT_STORE_ADD_ALWAYS, nullptr)) {
                CRYPT_DATA_BLOB out = {};
                if (PFXExportCertStoreEx(mem, &out, kPfxPassword, nullptr, EXPORT_PRIVATE_KEYS)) {
                    pfx.resize(out.cbData);
                    out.pbData = pfx.data();
                    ok = PFXExportCertStoreEx(mem, &out, kPfxPassword, nullptr, EXPORT_PRIVATE_KEYS) != FALSE;
                    pfx.resize(out.cbData);
                }
            }
            if (mem) CertCloseStore(mem, 0);
            CertFreeCertificateContext(cert);
        }
    }
    CryptReleaseContext(prov, 0);
    CryptAcquireContextW(&prov, container, nullptr, PROV_RSA_AES, CRYPT_DELETEKEYSET);
    if (!ok) err = "A signing certificate could not be created.";
    return ok;
}

// Load the player's key, creating it the first time. `keyFile`'s directory must exist.
bool EnsureSigningKey(const std::wstring& keyFile, std::vector<uint8_t>& pfx, std::string& err) {
    if (ReadWholeFile(keyFile, pfx) && !pfx.empty()) return true;
    if (!GenerateSigningKey(pfx, err)) return false;
    if (!WriteWholeFile(keyFile, pfx)) {
        // Not fatal: the apk can still be signed, it just will not update a previous one.
        // Say nothing here — the caller's log carries the path, and failing the whole
        // install over a cache file would be worse than the consequence.
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------
bool BuildAndroidApk(const void* templateApk, size_t templateBytes,
                     const std::wstring& keyFile,
                     const std::wstring& assetRoot,
                     const wchar_t* const* items, int itemCount,
                     const std::wstring& outApk,
                     ApkProgressFn cb, void* ctx, std::string& err) {
    std::vector<TplEntry> tpl;
    if (!ReadTemplate((const uint8_t*)templateApk, templateBytes, tpl, err)) return false;

    // Do this BEFORE writing 223 MB: a key problem should fail in a second, not a minute.
    std::vector<uint8_t> pfx;
    if (!EnsureSigningKey(keyFile, pfx, err)) return false;

    // What goes in the pack.
    std::vector<PakFile> files;
    for (int i = 0; i < itemCount; ++i) {
        std::wstring full = assetRoot + L"\\" + items[i];
        DWORD at = GetFileAttributesW(full.c_str());
        if (at == INVALID_FILE_ATTRIBUTES) continue;
        char nameA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, items[i], -1, nameA, sizeof nameA, nullptr, nullptr);
        if (at & FILE_ATTRIBUTE_DIRECTORY) {
            WalkDir(full, nameA, files);
        } else {
            PakFile f;
            f.full = full;
            f.rel = nameA;
            WIN32_FILE_ATTRIBUTE_DATA d;
            if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &d))
                f.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
            files.push_back(f);
        }
    }
    if (files.empty()) { err = "No game files were found to pack."; return false; }

    // The pack's index, and therefore its exact size, are known before a byte is written.
    std::vector<uint8_t> index;
    index.push_back('T'); index.push_back('J'); index.push_back('P'); index.push_back('K');
    Put32(index, 1);
    Put32(index, (uint32_t)files.size());
    uint64_t payload = 0;
    for (const PakFile& f : files) {
        Put16(index, (uint16_t)f.rel.size());
        index.insert(index.end(), f.rel.begin(), f.rel.end());
        for (int i = 0; i < 8; ++i) index.push_back((uint8_t)(f.size >> (i * 8)));
        payload += f.size;
    }
    const uint64_t pakSize = index.size() + payload;
    if (pakSize > 0xFFFFFFFFull) { err = "The game data is too large for one package."; return false; }

    OutFile out;
    if (!out.Open(outApk)) { err = "The Android package could not be created."; return false; }

    std::vector<OutEntry> dir;
    // name -> base64 SHA-256 of the entry's (uncompressed) bytes, for MANIFEST.MF
    std::vector<std::pair<std::string, std::string>> digests;

    // 1. the template's own entries, byte for byte
    for (const TplEntry& e : tpl) {
        if (!AddEntry(out, dir, e.name, e.data, e.size)) { err = "Writing the Android package failed."; return false; }
        std::vector<uint8_t> h;
        if (!Sha256Of(e.data, e.size, h)) { err = "Hashing the Android package failed."; return false; }
        digests.push_back({ e.name, Base64(h) });
    }

    // 2. assets/game.pak — streamed, so 223 MB never sits in memory. The local header goes
    //    down with a placeholder CRC and is patched once the bytes have been read.
    const std::string pakName = "assets/game.pak";
    const uint64_t pakLocalOfs = out.pos;
    {
        std::vector<uint8_t> hdr;
        LocalHeader(hdr, pakName, 0, pakSize);
        if (!out.Write(hdr)) { err = "Writing the Android package failed."; return false; }
    }
    Crc32 pakCrc;
    Sha256 pakSha;
    if (!pakSha.Begin()) { err = "Hashing the Android package failed."; return false; }
    pakCrc.Update(index.data(), index.size());
    pakSha.Update(index.data(), index.size());
    if (!out.Write(index)) { err = "Writing the Android package failed."; return false; }

    std::vector<uint8_t> buf(1u << 20);
    uint64_t done = 0;
    for (const PakFile& f : files) {
        HANDLE in = CreateFileW(f.full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in == INVALID_HANDLE_VALUE) { err = "A game file could not be read while packing."; return false; }
        uint64_t left = f.size;
        while (left) {
            DWORD want = (DWORD)(left < buf.size() ? left : buf.size());
            DWORD got = 0;
            if (!ReadFile(in, buf.data(), want, &got, nullptr) || got == 0) {
                CloseHandle(in); err = "A game file ended early while packing."; return false;
            }
            pakCrc.Update(buf.data(), got);
            pakSha.Update(buf.data(), got);
            if (!out.Write(buf.data(), got)) { CloseHandle(in); err = "Writing the Android package failed."; return false; }
            left -= got;
            done += got;
        }
        CloseHandle(in);
        if (cb && !cb(ctx, L"Packing the game into the Android app\x2026", done, payload)) {
            err = "cancelled"; return false;
        }
    }
    {
        const uint32_t crc = pakCrc.Final();
        if (!out.PatchAt(pakLocalOfs + 14, &crc, 4)) { err = "Writing the Android package failed."; return false; }
        OutEntry e; e.name = pakName; e.crc = crc; e.size = pakSize; e.localOfs = pakLocalOfs;
        dir.push_back(e);
        std::vector<uint8_t> h;
        if (!pakSha.Final(h)) { err = "Hashing the Android package failed."; return false; }
        digests.push_back({ pakName, Base64(h) });
    }

    // 3. the v1 signature, over everything above
    if (cb) cb(ctx, L"Signing the Android app\x2026", payload, payload);
    std::string manifest = "Manifest-Version: 1.0\r\nCreated-By: Tom & Jerry WOW installer\r\n\r\n";
    const size_t mainLen = manifest.size();
    for (const auto& d : digests) manifest += ManifestSection(d.first, d.second);

    std::vector<uint8_t> h;
    if (!Sha256Of(manifest.data(), manifest.size(), h)) { err = "Signing failed."; return false; }
    std::string sf = "Signature-Version: 1.0\r\nSHA-256-Digest-Manifest: " + Base64(h) + "\r\n";
    if (!Sha256Of(manifest.data(), mainLen, h)) { err = "Signing failed."; return false; }
    sf += "SHA-256-Digest-Manifest-Main-Attributes: " + Base64(h) + "\r\nCreated-By: Tom & Jerry WOW installer\r\n\r\n";
    {   // each .SF section digests that entry's MANIFEST SECTION TEXT, not the entry itself
        size_t at = mainLen;
        for (const auto& d : digests) {
            std::string sec = ManifestSection(d.first, d.second);
            if (!Sha256Of(manifest.data() + at, sec.size(), h)) { err = "Signing failed."; return false; }
            sf += ManifestSection(d.first, Base64(h));
            at += sec.size();
        }
    }
    std::vector<uint8_t> sfBytes(sf.begin(), sf.end());
    std::vector<uint8_t> p7;
    if (!Pkcs7Sign(pfx.data(), pfx.size(), kPfxPassword, sfBytes, p7, err)) return false;

    if (!AddEntry(out, dir, "META-INF/MANIFEST.MF", manifest.data(), manifest.size()) ||
        !AddEntry(out, dir, "META-INF/TJSIGN.SF", sf.data(), sf.size()) ||
        !AddEntry(out, dir, "META-INF/TJSIGN.RSA", p7.data(), p7.size())) {
        err = "Writing the Android package failed."; return false;
    }

    if (!WriteCentralDirectory(out, dir)) { err = "Writing the Android package failed."; return false; }
    out.Close();
    return true;
}

} // namespace tj::setup
