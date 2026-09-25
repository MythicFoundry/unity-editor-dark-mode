#if UNITY_EDITOR_WIN
namespace MythicFoundry.UnityEditorDarkMode
{
    using System;
    using System.Runtime.InteropServices;
    using UnityEditor;
    using UnityEngine;

    internal static class UnityEditorDarkModeBootstrap
    {
        private const string _LIBRARY_NAME = "UnityEditorDarkMode";
        private const double _INITIALIZATION_TIMEOUT_SECONDS = 10.0;

        private static double _initializationDeadline;

        [DllImport(_LIBRARY_NAME, CallingConvention = CallingConvention.Winapi, EntryPoint = "UnityEditorDarkMode_Initialize")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InitializeNative();

        [InitializeOnLoadMethod]
        private static void QueueInitialization()
        {
            if (Application.isBatchMode || AssetDatabase.IsAssetImportWorkerProcess()) return;

            _initializationDeadline = EditorApplication.timeSinceStartup + _INITIALIZATION_TIMEOUT_SECONDS;
            EditorApplication.update -= Initialize;
            EditorApplication.update += Initialize;
        }

        private static void Initialize()
        {
            try
            {
                if (InitializeNative())
                {
                    EditorApplication.update -= Initialize;
                    return;
                }
            }
            catch (DllNotFoundException exception)
            {
                EditorApplication.update -= Initialize;
                Debug.LogException(exception);
                return;
            }
            catch (EntryPointNotFoundException exception)
            {
                EditorApplication.update -= Initialize;
                Debug.LogException(exception);
                return;
            }

            if (EditorApplication.timeSinceStartup < _initializationDeadline) return;

            EditorApplication.update -= Initialize;
            Debug.LogError("Unity Editor Dark Mode could not attach to the Unity main window within 10 seconds.");
        }
    }
}
#endif
