//! The side of `nix::rs::call` that is in Rust: the error that the last
//! failed bridge function kept; see [`crate::future::bridge`].

use std::ffi::c_char;

use crate::future::{NixException, take_pending_exception};

/// `nix_rust_take_pending_exception` of `nix/rust/future-abi.h`.
///
/// # Safety
///
/// `message` must be valid for `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nix_rust_take_pending_exception(message: *const c_char, length: usize) -> *mut NixException {
    // SAFETY: The caller guarantees that `message` is valid for `length`
    // bytes.
    let message = unsafe { std::slice::from_raw_parts(message.cast::<u8>(), length) };
    take_pending_exception(message)
}

/// Every `extern "Rust"` function of every bridge in `src` returns a
/// `Result`, and its body is `bridge(...)`, so that no panic reaches cxx,
/// which would abort the process, and no error loses its value.
#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    /// The declarations in the `extern "Rust"` blocks of `source`, each as
    /// one line: `fn name(...) -> ...`.
    fn declarations(source: &str) -> Vec<String> {
        let mut result = Vec::new();
        let mut rest = source;
        while let Some(start) = rest.find("extern \"Rust\" {") {
            rest = &rest[start..];
            let end = block_end(rest);
            let block = &rest[..end];
            let mut current = String::new();
            for line in block.lines().skip(1) {
                let line = line.trim();
                if line.starts_with("//") || line.starts_with("#[") {
                    continue;
                }
                if current.is_empty() && !line.starts_with("fn ") && !line.starts_with("unsafe fn ") {
                    continue;
                }
                current.push_str(line);
                current.push(' ');
                if line.ends_with(';') {
                    result.push(std::mem::take(&mut current));
                }
            }
            rest = &rest[end..];
        }
        result
    }

    /// The length of the block that starts at the first `{` of `text`.
    fn block_end(text: &str) -> usize {
        let mut depth = 0;
        for (i, c) in text.char_indices() {
            match c {
                '{' => depth += 1,
                '}' => {
                    depth -= 1;
                    if depth == 0 {
                        return i + 1;
                    }
                }
                _ => {}
            }
        }
        text.len()
    }

    fn name(declaration: &str) -> &str {
        let after = declaration.split("fn ").nth(1).expect("a declaration has `fn `");
        &after[..after.find('(').expect("a declaration has arguments")]
    }

    /// The bodies of the functions called `name` in `source`, outside the
    /// bridge modules.
    fn bodies<'a>(source: &'a str, name: &str) -> Vec<&'a str> {
        let pattern = format!("fn {name}(");
        let mut result = Vec::new();
        let mut offset = 0;
        while let Some(found) = source[offset..].find(&pattern) {
            let at = offset + found;
            offset = at + pattern.len();
            let line_start = source[..at].rfind('\n').map_or(0, |i| i + 1);
            if source[line_start..at].trim_start().starts_with("//") {
                continue;
            }
            // The signature ends at the first `;` or `{` outside brackets,
            // such as the `;` of `[u8; 20]`. A declaration in a bridge ends
            // with `;`, a definition with `{`.
            let after = &source[at..];
            let mut depth = 0i32;
            let end = after.char_indices().find(|&(_, c)| {
                match c {
                    '(' | '[' => depth += 1,
                    ')' | ']' => depth -= 1,
                    _ => {}
                }
                depth == 0 && (c == ';' || c == '{')
            });
            if let Some((i, '{')) = end {
                result.push(after[i + 1..].trim_start());
            }
        }
        result
    }

    #[test]
    fn every_bridge_function_uses_bridge() {
        let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        let mut problems = Vec::new();
        let mut count = 0;
        for entry in fs::read_dir(&src).expect("src is readable") {
            let path = entry.expect("src is readable").path();
            if path.extension().is_none_or(|e| e != "rs") {
                continue;
            }
            let source = fs::read_to_string(&path).expect("a source file is readable");
            let file = path.file_name().expect("a file has a name").to_string_lossy();
            for declaration in declarations(&source) {
                count += 1;
                let name = name(&declaration);
                if !declaration.contains("-> Result<") {
                    problems.push(format!("{file}: `{name}` does not return `Result`"));
                }
                let bodies = bodies(&source, name);
                if bodies.is_empty() {
                    problems.push(format!("{file}: no body of `{name}`"));
                }
                for body in bodies {
                    if !body.starts_with("bridge(") {
                        problems.push(format!("{file}: the body of `{name}` is not `bridge(...)`"));
                    }
                }
            }
        }
        assert_eq!(problems, Vec::<String>::new());
        // The scan found the bridges at all: 15 when this test was written.
        assert!(count >= 15, "found only {count} bridge functions");
    }
}
