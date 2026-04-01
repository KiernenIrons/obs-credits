---
name: code-check
description: Review all staged/changed code for OBS plugin errors before committing
user_invocable: true
---

# Code Check - OBS Plugin Pre-Commit Review

You are a code reviewer specializing in OBS Studio plugin development. Perform a thorough review of all staged and modified code.

## Steps

1. **Gather changes**: Run `git diff --cached --name-only` to list staged files. If nothing is staged, run `git diff --name-only` for unstaged changes. If neither has results, report "No changes to review" and stop.

2. **Read each changed file** in full using the Read tool. Also run `git diff --cached` (or `git diff` if nothing staged) to see the exact lines changed.

3. **Review for the following categories of issues**, ordered by severity:

### CRITICAL (must block commit)
- **Compilation errors**: Syntax errors, missing includes, undeclared identifiers, type mismatches
- **Graphics thread violations**: Any `gs_*` call outside `video_render` or `obs_enter_graphics()`/`obs_leave_graphics()` blocks
- **Use-after-free**: Accessing `obs_data_t`, `obs_source_t`, or any ref-counted object after calling its `_release()` function
- **Double free / over-release**: Calling `_release()` on objects not owned (not created or addref'd)
- **Missing OBS_DECLARE_MODULE()**: Plugin entry file must have this macro
- **Missing null checks**: On return values from `obs_data_get_*`, `obs_source_get_*`, `obs_get_*` families
- **Memory leaks**: Allocated with `bmalloc`/`bstrdup`/`bzalloc`/`obs_data_create` but never freed/released
- **Thread safety**: Shared mutable state accessed from `update` callback without mutex protection

### WARNING (should fix but don't block)
- **Using malloc/free/strdup** instead of OBS equivalents (`bmalloc`, `bfree`, `bstrdup`)
- **Using printf/stdout/stderr** instead of `blog()`
- **Missing localization**: User-visible strings not wrapped in `obs_module_text()`
- **Missing symbol prefix**: Public functions not prefixed with the plugin name
- **Platform-specific code** without `#ifdef` guards
- **Unused variables or dead code** in changed sections
- **Missing `obs_module_unload`** cleanup for registered resources

### INFO (suggestions)
- Code style inconsistencies (naming conventions, indentation)
- Opportunities to use OBS helper functions instead of manual implementation
- Missing `OBS_MODULE_USE_DEFAULT_LOCALE` when locale files exist

4. **Report findings** in this format:

```
## Code Check Results

### CRITICAL ❌ (N issues)
- [file.c:42] Description of the critical issue
  → Suggested fix

### WARNING ⚠️ (N issues)
- [file.c:15] Description of the warning
  → Suggested fix

### INFO ℹ️ (N issues)
- [file.c:7] Description of the suggestion

### Summary
- Files reviewed: N
- Critical: N | Warnings: N | Info: N
- Verdict: PASS ✅ / FAIL ❌
```

5. **Verdict logic**:
   - Any CRITICAL issue → **FAIL** (recommend blocking the commit)
   - Only WARNING or INFO → **PASS** (safe to commit, but note the warnings)

If the verdict is FAIL, clearly state: **"Fix the critical issues above before committing."**
