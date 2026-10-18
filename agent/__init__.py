"""B8: the delegate sandbox (BUILD_B_GATEWAY.md §3 B8).

An LLM delegate that writes MPL from a one-paragraph brief and submits it with an author key,
the small client library it uses, and the Daytona sandbox it runs in. The delegate holds an
author key only: it can submit, read and ask for repair. It cannot deploy, cancel or move money.
"""
