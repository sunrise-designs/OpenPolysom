import sys
from pathlib import Path

# src_python/*.py modules import each other with bare names (e.g.
# `from signal_processing import count_plm`), matching the rest of the repo's
# flat-script convention — make src_python/ importable the same way for tests.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
