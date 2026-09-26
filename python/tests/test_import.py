import jarvis


def test_native_module_imports() -> None:
    assert jarvis._core.__doc__ == "jarvis native extension: model types and event logs"
    assert jarvis.model.Price is jarvis._core.model.Price
    assert jarvis.log.EventLogReader is jarvis._core.log.EventLogReader


def test_build_info_identifies_the_build() -> None:
    info = jarvis.build_info()
    assert set(info) == {"version", "git_commit", "compiler", "platform", "live_enabled"}
    assert info["version"]
    assert info["git_commit"]
    assert info["platform"]
    assert isinstance(info["live_enabled"], bool)
