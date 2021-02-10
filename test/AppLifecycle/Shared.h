// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.
#pragma once

namespace ProjectReunionCppTest
{
    winrt::Windows::Storage::StorageFile CreateDocFile(std::wstring filename);
    winrt::Windows::Storage::StorageFile OpenDocFile(std::wstring filename);
    wil::unique_handle Execute(const std::wstring& command, const std::wstring& args,
        const std::wstring& directory);
    void RunCertUtil(const std::wstring& path, bool removeCert = false);
    void InstallPackage(const std::wstring& packagePath);
    void UninstallPackage(const std::wstring& packageFullName);
    wil::unique_event CreateTestEvent(const std::wstring& eventName);
    void WaitForEvent(const wil::unique_event& successEvent, const wil::unique_event& failedEvent);
    const std::wstring GetDeploymentDir();
    void WriteContentFile(std::wstring filename);
    void DeleteContentFile(std::wstring filename);

    const std::wstring g_deploymentDir = GetDeploymentDir();

    using namespace winrt::Windows::ApplicationModel::Activation;
    class TestArgs : public winrt::implements<TestArgs, IActivatedEventArgs, ILaunchActivatedEventArgs>
    {
    public:
        TestArgs(const std::wstring& args) : m_args(args)
        {
            m_kind = ActivationKind::Launch;
        }

        // IActivatedEventArgs
        ActivationKind Kind()
        {
            return m_kind;
        }

        ApplicationExecutionState PreviousExecutionState()
        {
            return m_previousState;
        }

        SplashScreen SplashScreen()
        {
            return m_splashScreen;
        }

        // ILaunchActivatedEventArgs
        winrt::hstring Arguments()
        {
            return m_args.c_str();
        }

        winrt::hstring TileId()
        {
            // This implementation is only used for Win32 which don't support secondary tiles.
            return L"";
        }

    private:
        TestArgs() = default;

        ActivationKind m_kind = ActivationKind::Launch;
        ApplicationExecutionState m_previousState;
        winrt::Windows::ApplicationModel::Activation::SplashScreen m_splashScreen{ nullptr };
        std::wstring m_args;
    };
}
