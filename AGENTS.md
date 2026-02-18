# Agent notes

## C++ (MSVC) forward declarations

When editing existing `.cpp`/`.inl` files, MSVC will error if a function is used before it is declared/defined (unlike some compilers/configs that may have previously tolerated ordering differences).

Rule of thumb:
- If you add a new helper that calls an existing `static` function that is defined later in the file, add a forward declaration near the top of the file (or reorder definitions) so the identifier is visible.
- This commonly comes up when inserting new helpers mid-file (e.g. adding a hook like `Hook_GetCursorPos` that wants to call `TryGetScaledInfoForHwnd`).

Prefer:
- Minimal forward declarations for the few functions needed, placed near the other function-pointer typedefs/globals.
- Or, if practical, move the callee definition above the caller.
