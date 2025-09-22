# Hacking on the C API

[C API user introduction](../manual/source/c-api.md) in the manual.

## API stability

- We do not break ABI.
- We do not expose any fields in our structs.
- We do not expose any unions.
- Enum values can not be changed or renamed, but enums can be extended.
- We do not rename types, because it breaks downstream bindings code generation.

## Naming convention

The naming convention for functions and types is `nix_lower_snake_case`.

Macros are `NIX_UPPER_CAMEL_CASE`.
`enum` value names additionally include the type name, e.g. `NIX_TYPE_INT`, ideally directly derived from the type name for consistency.

Some `CamelCase` types are left over from the early days of the C API.

We may provide a configuration macro that switches these old types to the new convention, opt-in.

## Memory management patterns

### Garbage-Collected Objects

Objects managed by Nix's garbage collector use reference counting for C API interop:

```c
// Allocation automatically increments refcount
nix_value * value = nix_alloc_value(ctx, state);
PrimOp * primop = nix_alloc_primop(ctx, callback, 1, "name", args, "doc", NULL);

// Getter functions return new references (refcount already incremented)
nix_value * attr = nix_get_attr_byname(ctx, attrset, state, "foo");
nix_value * listItem = nix_get_list_byidx(ctx, list, state, 0);

// Always decref when done
nix_value_decref(ctx, value);
nix_gc_decref(ctx, primop);  // TODO: provide specialized function
nix_value_decref(ctx, attr);
nix_value_decref(ctx, listItem);
```

**Move semantics:** Some functions consume objects and transfer ownership:

```c
PrimOp * p = nix_alloc_primop(ctx, callback, 1, "increment", args, "doc", NULL);
nix_register_primop(ctx, p);  // Moves content into global registry
nix_gc_decref(ctx, p);        // Still must decref the wrapper
```

### Builder Objects

Builders are not garbage collected and have their own free functions:

```c
// Create builders with specific capacity
ListBuilder * listBuilder = nix_make_list_builder(ctx, state, 3);
BindingsBuilder * attrBuilder = nix_make_bindings_builder(ctx, state, 2);

// Use builders to construct values
nix_list_builder_insert(ctx, listBuilder, 0, value1);
nix_bindings_builder_insert(ctx, attrBuilder, "key", value2);

// Create the final values (this invalidates the builders)
nix_make_list(ctx, listBuilder, resultList);
nix_make_attrs(ctx, resultAttrs, attrBuilder);

// Free builders (must be done after creation)
nix_list_builder_free(listBuilder);
nix_bindings_builder_free(attrBuilder);
```

### Special-Purpose Objects

Some objects have their own management:

```c
// Realized strings must be freed with their specific function
nix_realised_string * realized = nix_string_realise(ctx, state, stringValue, false);
// ... use realized string ...
nix_realised_string_free(realized);
```

### Borrowed Pointers

Some functions return pointers owned by other objects:

```c
// String is owned by the EvalState, no cleanup needed
const char * attrName = nix_get_attr_name_byidx(ctx, attrset, state, 0);
```

## Documentation guidelines

Doxygen documentation comments are required for each documentable item in the C API.

Documentation comments are **reference documentation** in line with the [Diataxis definition and guidelines](https://diataxis.fr/reference/) (8 min) which is required reading.

### Template

```c
/** @brief Describe briefly in imperative mood
 *
 * @param[in] context Optional, stores error information
 * @param[in] param Description
 * @return Description, or NULL on error
 */
```

### Key improvements over status quo

**Brief:** Start with imperative verb. "Get list length" not "Gets the length".

**Ownership and lifetime:** Be specific about when pointers become invalid:
- "Valid until [parent object] is freed"
- "Valid while [specific condition] holds"  
- "Call [specific_function] to release your reference"
- "Caller takes ownership, must call [specific_free_function]"

**Error handling:** Document when NULL is returned and mention checking context.

**Parameters:** Use `[in]`, `[out]`, `[in,out]` consistently. Note whether NULL is allowed.

**Integer indexing functions:** Document bounds. "Must be less than nix_get_X_size()".

**Notes and warnings:** Use `@note` for important behaviors that users might miss.

**Common parameter patterns:**
- `context` - "Optional, stores error information"  
- `state` - "Evaluator state, must not be NULL"
- `value` - Specify the expected type and NULL policy

**Function grouping:** Use `@ingroup groupname` in each function to declare group membership.

**Struct documentation:** For struct typedefs, especially opaque types, use `@struct structname` to ensure proper cross-referencing in generated documentation.

**Deprecation:** Use **both** `NIX_DEPRECATED("message")` **and** `@deprecated` in Doxygen comments. The macro provides compile-time warnings; the Doxygen tag appears in generated docs.

**Enum documentation:** Document both the enum type and individual values. Use `@enum` for the type name and document each value's meaning.

**Macro documentation:** Document `#define` constants and function-like macros. Use `@def` for the macro name and `@param` for parameters in function-like macros.

**Code examples:** Use ```c code blocks for usage examples in documentation. Show typical usage patterns and common use cases. Prefer ```c over `@code/@endcode` for better syntax highlighting.

**Memory validity and responsibilities:** Specify when pointers become invalid and what actions callers must take. Use concrete phrases like "Call nix_value_decref() when done", "Valid until context is freed", "Pointer invalidated when X is called", "Must call Y to release".

**Forward declarations:** Document structs and enums only at their primary declaration site. Forward declarations should have a comment referring to their documentation's location (e.g., `// Documented in ./nix_api_expr.h`).

**Manual integration:** Link to official documentation when available, especially for core language concepts. Use `@see https://nix.dev/...` links for enum values and key concepts. Use stable or latest URLs, not versioned ones.

**Group organization:** Define documentation groups before the items that use them. Use hierarchical groups (parent groups with child groups using `@ingroup`) for better organization.

**Reference documentation:** This documentation is consulted, not read. It must have a high level of detail and precision as per Diataxis guidelines.

**Additional tags:** Use `@since`, `@warning`, `@todo`, and `@file` as appropriate. Use `@warning` for critical safety issues, `@todo` for known limitations.

### Memory Management Documentation Patterns

**Always read implementation before documenting:** Check the actual function implementation to understand the precise memory management behavior rather than making assumptions.

**Be concrete, not vague:** Replace vague phrases like "Owned by the GC" with specific actions:
- ✅ "Call nix_gc_decref() when done with the pointer"
- ✅ "Call nix_list_builder_free() when done"
- ❌ "Owned by the garbage collector"
- ❌ "Make sure to unref when done"

**Distinguish memory management patterns:**
- **GC-managed objects** (nix_value, PrimOp, ExternalValue): Use `nix_gc_decref()`
- **Builder objects** (ListBuilder, BindingsBuilder): Use dedicated free functions
- **Realized strings** (nix_realised_string): Use `nix_realised_string_free()`
- **Borrowed pointers** (const char * from nix_get_attr_name_byidx): Document ownership (e.g., "Owned by the EvalState")

**Document move semantics clearly:** When functions like `nix_register_primop()` move content:
```c
/** @brief Register a primop in the global evaluator
 *
 * Moves your PrimOp content into the global evaluator registry, 
 * meaning your input PrimOp pointer is no longer usable.
 * You are free to remove your references to it,
 * after which it will be garbage collected.
 */
```

**Clarify builder usage patterns:** Builders remain valid after use but are typically freed immediately:
```c
/** @brief Create a list from a list builder
 * @param[in] list_builder list builder to use. Make sure to unref this afterwards.
 */
```

**Document return value ownership immediately:**
```c
 * @return NULL if failed, or a new nix_realised_string, which must be freed with nix_realised_string_free
```

### Examples

#### Group

Group declaration `value_extract`, part of group `value`. Note the absence of `@{` and `@}`. We use `@ingroup` exclusively.

```c
/** @defgroup value_extract Value Extraction
 * @brief Functions for extracting data from Nix values
 * @ingroup value
 */
```

#### Struct Documentation

```c
/** @brief Brief description of the struct
 *
 * Detailed description explaining the struct's purpose,
 * lifetime, and usage patterns.
 *
 * @struct struct_name
 * @see related_function
 * @ingroup group_name
 */
typedef struct my_struct my_struct;
```

#### Enum Documentation

```c
/** @brief Represents the state and type of a Nix value
 *
 * Used to determine what kind of data a nix_value contains
 * before accessing type-specific functions.
 *
 * @enum nix_value_type
 * @ingroup value
 */
typedef enum {
    /** An unevaluated value
     *
     * Their state is mutable, unlike that of the other types.
     */
    NIX_TYPE_THUNK,
    /**
     * A 64 bit signed integer.
     */
    NIX_TYPE_INT,
} nix_value_type;
```

#### Macro Documentation

```c
/** @brief Convenience macro for calling Nix functions with multiple arguments
 * @def NIX_VALUE_CALL
 * @ingroup value_create
 *
 * Technically these are functions that return functions. It is common for Nix
 * functions to be curried, so this function is useful for calling them.
 *
 * @param[out] context Optional, stores error information
 * @param[in] state The state of the evaluation
 * @param[out] value The result of the function call
 * @param[in] fn The Nix function to call
 * @param[in] ... The arguments to pass to the function
 *
 * @see nix_value_call_multi
 */
#define NIX_VALUE_CALL(context, state, value, fn, ...)                      \
    do {                                                                    \
        nix_value * args_array[] = {__VA_ARGS__};                           \
        size_t nargs = sizeof(args_array) / sizeof(args_array[0]);          \
        nix_value_call_multi(context, state, fn, nargs, args_array, value); \
    } while (0)
```

#### Function Documentation


Function declaration:

```c
/** @brief Get attribute by index
 *
 * Retrieves both the attribute name and value at the specified index.
 * Any other important behaviors are described here.
 *
 * @note This particular behavior needs special attention.
 *
 * @param[out] context Optional, stores error information.
 * @param[in] value Attribute set value, must not be NULL.
 * @param[in] state Evaluator state, must not be NULL.
 * @param[in] i Index, must be less than nix_get_attrs_size().
 * @param[out] name Receives string pointer. Returned string is valid until context or state are released.
 *
 * @return Attribute value. NULL if out of bounds. Otherwise, returned value is valid until state is released or value is released. Release with nix_value_decref.
 *
 * @see nix_get_attrs_size, nix_get_attr_name_byidx
 * @ingroup value_extract
 *
 * @since Nix 2.22
 */
nix_value * nix_get_attr_byidx(
    nix_c_context * context,
    nix_value * value,
    EvalState * state, 
    unsigned int i,
    const char ** name);
```
