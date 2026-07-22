"""Public Maya entry point for NitroPoly."""


def show(reload_module=False):
    """Show the single NitroPoly window, optionally reloading during development."""
    import NitroPoly

    if reload_module:
        try:
            reload_function = reload
        except NameError:
            from importlib import reload as reload_function
        NitroPoly = reload_function(NitroPoly)
    return NitroPoly.main()
