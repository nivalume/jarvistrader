def test_native_module_imports() -> None:
    import jarvis

    assert jarvis._core.__doc__ == "jarvis native extension scaffold"
