"""`python -m agent` is the same as `python -m agent.delegate`."""
import sys

from .delegate import main

sys.exit(main())
