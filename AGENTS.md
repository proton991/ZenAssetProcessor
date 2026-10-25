# Global coding preferences

Apply these rules when writing or modifying C++ code in any project.

1. Use a single return statement per function, at the end; avoid early returns. Functions that need no explicit return, such as void functions, may omit it.
2. Do not use the `auto` keyword except for iterator types. Use explicit types elsewhere.
3. Use lambdas only when necessary, and keep them short. Use named functions or methods for long logic.
4. Prefer the current project's own containers when a suitable one exists. In ZenEngine, use `HeapVector` instead of `std::vector`; apply the same preference to other available project containers.
5. Format C++ files using the project's `.clang-format` file. VSCode's format-on-save feature can be used to apply this formatting automatically.
6. Reduce code duplication, including duplicated functions and duplicated logic. Reuse existing code or extract shared logic into suitable functions or methods.
7. Do not throw exceptions for error handling. Use error codes or status results, log error messages, and use assertions for violated invariants as appropriate. Handle failures explicitly so execution does not continue with invalid data.

The user may extend this list later. Preserve these rules when adding new preferences.
