def test_native_module_imports() -> None:
    import jarvis

    assert jarvis._core.__doc__ == "jarvis native extension scaffold"


def test_build_info_identifies_the_build() -> None:
    import jarvis

    info = jarvis._core.build_info()
    assert set(info) == {"version", "git_commit", "compiler", "live_enabled"}
    assert info["version"]
    assert info["git_commit"]
    assert isinstance(info["live_enabled"], bool)
