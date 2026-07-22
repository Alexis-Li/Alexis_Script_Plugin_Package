try:
    reload
except NameError:
    from importlib import reload

import NitroPoly

reload(NitroPoly)
NitroPoly.main()
