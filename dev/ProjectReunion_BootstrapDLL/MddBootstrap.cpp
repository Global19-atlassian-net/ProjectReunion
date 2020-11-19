﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"

#include "MddBootstrap.h"
#include "MddBootstrapTest.h"

#include "IDynamicDependencyLifetimeManager.h"

HRESULT GetFrameworkPackageInfoForPackage(PCWSTR packageFullName, const PACKAGE_INFO*& frameworkPackageInfo, wil::unique_cotaskmem_ptr<BYTE[]>& packageInfoBuffer);
HRESULT AddFrameworkToPath(PCWSTR  path);
CLSID FindDDLM(const PACKAGE_VERSION minVersion);
CLSID GetClsid(const winrt::Windows::ApplicationModel::AppExtensions::AppExtension appExtension);

IDynamicDependencyLifetimeManager* g_lifetimeManager{};
DLL_DIRECTORY_COOKIE g_dllDirectoryCookie{};
wil::unique_cotaskmem_string g_frameworkPath;

std::wstring g_test_ddlmPackageNamePrefix;
std::wstring g_test_ddlmPackagePublisherId;

inline winrt::Windows::System::ProcessorArchitecture GetCurrentArchitecture()
{
#if defined(_M_X64)
    return winrt::Windows::System::ProcessorArchitecture::X64;
#elif defined(_M_IX86)
    return winrt::Windows::System::ProcessorArchitecture::X86;
#elif defined(_M_ARM64)
    return winrt::Windows::System::ProcessorArchitecture::Arm64;
#elif defined(_M_ARM)
    return winrt::Windows::System::ProcessorArchitecture::Arm;
#else
#   error "Unknown processor architecture"
#endif
}

inline PCWSTR GetCurrentArchitectureAsString()
{
#if defined(_M_X64)
    return L"x64";
#elif defined(_M_IX86)
    return L"x86";
#elif defined(_M_ARM64)
    return L"arm64";
#elif defined(_M_ARM)
    return L"arm";
#else
#   error "Unknown processor architecture"
#endif
}

inline winrt::Windows::System::ProcessorArchitecture ParseArchitecture(PCWSTR architecture)
{
    if (CompareStringOrdinal(architecture, -1, L"x64", -1, TRUE) == CSTR_EQUAL)
    {
        return winrt::Windows::System::ProcessorArchitecture::X64;
    }
    else if (CompareStringOrdinal(architecture, -1, L"x86", -1, TRUE) == CSTR_EQUAL)
    {
        return winrt::Windows::System::ProcessorArchitecture::X86;
    }
    else if (CompareStringOrdinal(architecture, -1, L"arm64", -1, TRUE) == CSTR_EQUAL)
    {
    return winrt::Windows::System::ProcessorArchitecture::Arm64;
    }
    else if (CompareStringOrdinal(architecture, -1, L"arm", -1, TRUE) == CSTR_EQUAL)
    {
    return winrt::Windows::System::ProcessorArchitecture::Arm;
    }
    else
    {
        return winrt::Windows::System::ProcessorArchitecture::Unknown;
    }
}

STDAPI MddBootstrapInitialize(
    const PACKAGE_VERSION minVersion) noexcept try
{
    FAIL_FAST_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), g_lifetimeManager != nullptr);
    FAIL_FAST_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), g_dllDirectoryCookie != 0);
    FAIL_FAST_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), g_frameworkPath);

    const auto appDynamicDependencyLifetimeManagerClsid{ FindDDLM(minVersion) };

    wil::com_ptr_nothrow<IDynamicDependencyLifetimeManager> lifetimeManager(wil::CoCreateInstance<IDynamicDependencyLifetimeManager>(appDynamicDependencyLifetimeManagerClsid, CLSCTX_LOCAL_SERVER));

    THROW_IF_FAILED(lifetimeManager->Initialize());

    wil::unique_cotaskmem_string packageFullName;
    THROW_IF_FAILED(lifetimeManager->GetPackageFullName(&packageFullName));

    wil::unique_cotaskmem_ptr<BYTE[]> packageInfoBuffer;
    const PACKAGE_INFO* frameworkPackageInfo{};
    THROW_IF_FAILED(GetFrameworkPackageInfoForPackage(packageFullName.get(), frameworkPackageInfo, packageInfoBuffer));

    THROW_IF_FAILED(AddFrameworkToPath(frameworkPackageInfo->path));

    g_lifetimeManager = lifetimeManager.detach();
    return S_OK;
}
CATCH_RETURN();

STDAPI_(void) MddBootstrapShutdown() noexcept
{
    if (g_lifetimeManager)
    {
        (void)LOG_IF_FAILED(g_lifetimeManager->Shutdown());
        g_lifetimeManager->Release();
        g_lifetimeManager = nullptr;
    }

    if (g_dllDirectoryCookie != 0)
    {
        RemoveDllDirectory(g_dllDirectoryCookie);
        g_dllDirectoryCookie = 0;
    }

    if (g_frameworkPath)
    {
        //TODO: Revisit once MddAddPackageDependency is lit up
        wil::unique_cotaskmem_string path;
        const auto hr{ wil::TryGetEnvironmentVariableW(L"PATH", path) };
        if (SUCCEEDED(hr) && path)
        {
            const auto frameworkPathLength{ wcslen(g_frameworkPath.get()) };
            PCWSTR pathWithoutFrameworkPath{ g_frameworkPath.get() + frameworkPathLength };
            if (*pathWithoutFrameworkPath == L';')
            {
                ++pathWithoutFrameworkPath;
            }
            (void)LOG_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(L"PATH", pathWithoutFrameworkPath));
        }

        g_frameworkPath.reset();
    }
}

STDAPI MddBootstrapTestInitialize(
    _In_ PCWSTR ddlmPackageNamePrefix,
    _In_ PCWSTR ddlPackagePublisherId) noexcept try
{
    RETURN_HR_IF(E_INVALIDARG, !ddlmPackageNamePrefix);
    RETURN_HR_IF(E_INVALIDARG, *ddlmPackageNamePrefix == L'0');
    RETURN_HR_IF(E_INVALIDARG, !ddlPackagePublisherId);
    RETURN_HR_IF(E_INVALIDARG, *ddlPackagePublisherId == L'0');

    g_test_ddlmPackageNamePrefix = ddlmPackageNamePrefix;
    g_test_ddlmPackagePublisherId = ddlPackagePublisherId;
    return S_OK;
} CATCH_RETURN();

//TODO:Change error handle to exceptions
/// Determine the path for the Project Reunion Framework package
HRESULT GetFrameworkPackageInfoForPackage(PCWSTR packageFullName, const PACKAGE_INFO*& frameworkPackageInfo, wil::unique_cotaskmem_ptr<BYTE[]>& packageInfoBuffer)
{
    frameworkPackageInfo = nullptr;
    packageInfoBuffer.reset();

    // We need to determine the exact Project Reunion Framework package
    // in the sidecar package's dependencies, as resolved by Windows.
    // A user can have multiple framework packages in a family registered
    // at a time, for multiple reasons:
    //
    //   * Multiple Architectures -- x86/x64 on an x64 machine, x86/arm/arm64/x86ona64 on an arm64 machine, etc
    //   * Multiple Versions -- v1.0.0.0 in use by processes running as pkg1 and v1.0.0.1 in use by runnings running as pkg2
    //                          or v1.0.0.0 in use by running processes and v1.0.0.1 in package graphs for packages w/no running process
    //
    // Thus FindPackagesByPackageFamily(pkgfamilyname,...) and PackageManager.FindPackages(user="", pkgfamilyname) could be ambiguous.
    // We need the actual dependency graph known to Windows for the sidecar package where we got our LifetimeManager.
    // That leaves us few options:
    //
    //   * PackageManager.FindPackage(user="", lifetimeManager->GetPackageFullName()).Dependencies
    //   * GetPackageInfo(OpenPackageInfoByFullName(lifetimeManager->GetPackageFullName())
    //
    // We'll go with the latter as the simpler (no COM/WinRT) and more performant solution.

    // Fetch the package graph for the package (per packageFullName)
    wil::unique_package_info_reference packageInfoReference;
    RETURN_IF_WIN32_ERROR(OpenPackageInfoByFullName(packageFullName, 0, &packageInfoReference));
    UINT32 bufferLength{};
    UINT32 packageInfoCount{};
    const auto hr{ HRESULT_FROM_WIN32(GetPackageInfo(packageInfoReference.get(), PACKAGE_FILTER_DIRECT, &bufferLength, nullptr, &packageInfoCount)) };
    RETURN_HR_IF(hr, hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    RETURN_HR_IF(E_UNEXPECTED, packageInfoCount == 0);
    auto buffer{ wil::make_unique_cotaskmem_nothrow<BYTE[]>(bufferLength) };
    RETURN_IF_NULL_ALLOC(buffer);
    RETURN_IF_WIN32_ERROR(GetPackageInfo(packageInfoReference.get(), PACKAGE_FILTER_DIRECT, &bufferLength, buffer.get(), &packageInfoCount));

    // Find the Project Reunion Framework package in the package graph to determine its path
    const PACKAGE_INFO* packageInfo{ reinterpret_cast<const PACKAGE_INFO*>(buffer.get()) };
    for (size_t index = 0; index < packageInfoCount; ++index, ++packageInfo)
    {
        PCWSTR frameworkPackageFamilyName{ L"Microsoft.ProjectReunion.Framework_8wekyb3d8bbwe" };
        if (CompareStringOrdinal(packageInfo->packageFamilyName, -1, frameworkPackageFamilyName, -1, TRUE) == CSTR_EQUAL)
        {
            packageInfoBuffer = std::move(buffer);
            frameworkPackageInfo = packageInfo;
            return S_OK;
        }
    }

    // We didn't find the framework package as a dependency of the package
    // providing the LifetimeManager.
    //
    // Verify the package providing the LifetimeManager declares
    // <PackageDependency> on the Project Reunion Framework package.
    RETURN_WIN32(APPMODEL_ERROR_PACKAGE_NOT_AVAILABLE);
}

//TODO:Change error handle to exceptions
HRESULT AddFrameworkToPath(PCWSTR frameworkPath)
{
    // Add the framework to the Loader's DllDirectory list
    wil::unique_dll_directory_cookie dllDirectoryCookie{ AddDllDirectory(frameworkPath) };
    RETURN_LAST_ERROR_IF_NULL(dllDirectoryCookie);

    // Make a copy of the framework path to save for later
    wil::unique_cotaskmem_string frameworkPathCopy(wil::make_cotaskmem_string_nothrow(frameworkPath));
    RETURN_IF_NULL_ALLOC(frameworkPathCopy);

    // Add the framework the the PATH environment variable
    wil::unique_cotaskmem_string path;
    RETURN_IF_FAILED(wil::GetEnvironmentVariableW(L"PATH", path));
    if (path)
    {
        // PATH = frameworkPath + ";" + path
        wil::unique_cotaskmem_string newPath;
        RETURN_IF_FAILED(wil::str_concat_nothrow(newPath, frameworkPathCopy, L";", path));
        RETURN_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(L"PATH", newPath.get()));
    }
    else
    {
        const auto lastError{ GetLastError() };
        RETURN_HR_IF(HRESULT_FROM_WIN32(lastError), lastError != ERROR_ENVVAR_NOT_FOUND);
        RETURN_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(L"PATH", frameworkPath));
    }

    g_frameworkPath = std::move(frameworkPathCopy);
    g_dllDirectoryCookie = dllDirectoryCookie.release();
    dllDirectoryCookie = 0;
    return S_OK;
}

CLSID FindDDLM(const PACKAGE_VERSION minVersion)
{
    // Find the best fit
    bool foundAny{};
    PACKAGE_VERSION bestFitVersion{};
    CLSID bestFitClsid{};

    // Look for windows.appExtension with name="com.microsoft.projectreunion.ddlm.<majorversion>.<architecture>"
    WCHAR appExtensionName[100]{};
    wsprintf(appExtensionName, L"com.microsoft.projectreunion.ddlm.%hu.%s", minVersion.Major, GetCurrentArchitectureAsString());

    auto catalog{ winrt::Windows::ApplicationModel::AppExtensions::AppExtensionCatalog::Open(appExtensionName) };
    auto appExtensions{ catalog.FindAllAsync().get() };
    for (auto appExtension : appExtensions)
    {
        // Check the package identity against the package identity test qualifiers (if any)
        if (!g_test_ddlmPackageNamePrefix.empty())
        {
            const auto packageId{ appExtension.Package().Id() };
            std::wstring name{ packageId.Name().c_str() };
            if ((name.rfind(g_test_ddlmPackageNamePrefix.c_str(), 0) != 0) ||
                (CompareStringOrdinal(packageId.PublisherId().c_str(), -1, g_test_ddlmPackagePublisherId.c_str(), -1, TRUE) != CSTR_EQUAL))
            {
                // The package's Name prefix or PublisherId don't match the expected value. Skip it
                continue;
            }
        }

        // appExtension.Id == "ddlm-<major.minor.build.revision>-<architecture>"
        const auto id{ appExtension.Id() };
        PACKAGE_VERSION version{};
        WCHAR architectureAsString[9 + 1]{};
        const auto maxIdLength{ ARRAYSIZE(L"ddlm-12345.12345.12345.12345.abcdefghi") - 1 }; // -1 for length not counting null-terminator
        if ((id.size() >= maxIdLength) ||
            (swscanf_s(id.c_str(), L"ddlm-%hu.%hu.%hu.%hu-%9s", &version.Major, &version.Minor, &version.Build, &version.Revision, architectureAsString, static_cast<unsigned>(ARRAYSIZE(architectureAsString))) != 5))
        {
            (void)LOG_HR_MSG(ERROR_INVALID_DATA, "%ls", id.c_str());
            continue;
        }

        // Does the architecture match?
        const auto architecture{ ParseArchitecture(architectureAsString) };
        if (architecture != GetCurrentArchitecture())
        {
            continue;
        }

        // Do we yet to have a package under consideration?
        if (!foundAny)
        {
            bestFitVersion = version;
            bestFitClsid = GetClsid(appExtension);
            foundAny = true;
            continue;
        }

        // Do we already have a higher version under consideration?
        if (bestFitVersion.Version < version.Version)
        {
            bestFitVersion = version;
            bestFitClsid = GetClsid(appExtension);
            continue;
        }
    }
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NO_MATCH), !foundAny);
    return bestFitClsid;
}

CLSID GetClsid(const winrt::Windows::ApplicationModel::AppExtensions::AppExtension appExtension)
{
    const auto properties{ appExtension.GetExtensionPropertiesAsync().get() };
    auto propertiesClsid{ properties.Lookup(L"CLSID").as<winrt::Windows::Foundation::Collections::IPropertySet>() };
    auto value{ propertiesClsid.Lookup(L"#text").as<winrt::Windows::Foundation::IPropertyValue>() };
    THROW_HR_IF_NULL(E_UNEXPECTED, value);
    THROW_HR_IF(E_UNEXPECTED, value.Type() != winrt::Windows::Foundation::PropertyType::String);
    const auto text{ value.GetString() };

    // Convert the CLSID as a string to a CLSID as a GUID
    // Problem: CLSIDFromString() also does a lookup for a registered object by the CLSID.
    //          We just want the string->GUID conversion, not any additional work.
    // Workaround this by using UuidFromString()

    // Problem: UuidFromString() takes a RPC_WSTR but that's defined as unsigned short*
    //          unless RPC_USE_NATIVE_WCHAR is defined.
    // Workaround this with casts. Include some asserts to verify we're not misusing memory.

    auto textString{ const_cast<PWSTR>(text.c_str()) };
    auto textRpcString{ reinterpret_cast<RPC_WSTR>(textString) };

    static_assert(sizeof(textString) == sizeof(textRpcString));
    static_assert(sizeof(textString[0]) == sizeof(textRpcString[0]));

    UUID clsid{};
    THROW_IF_WIN32_ERROR(UuidFromStringW(textRpcString, &clsid));
    return clsid;
}