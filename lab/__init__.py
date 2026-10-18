"""The lab: nine live tests of the bounded key, served by the gateway process (lab/CONTRACT.md).

What is real: every test runs against the gateway in-process (its routes, its ledger, its bank
adapter) and the venue the gateway was built with (Build A's binaries when VENUE=real, the fake
venue otherwise). Every number in a result is read back from the gateway or the venue, never
from a test's own bookkeeping. What is simulated is labelled in the result itself
(`venue: fake`, `route: stub`, `simulated: true`); nothing is faked to fill a card.
"""
