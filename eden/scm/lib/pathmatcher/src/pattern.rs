/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

use std::path::Path;
use std::str::FromStr;

use anyhow::Result;

use crate::error::Error;
use crate::expand_curly_brackets;
use crate::normalize_glob;
use crate::plain_to_glob;
use crate::utils::make_glob_recursive;

#[derive(Debug, PartialEq, Copy, Clone, Hash, Eq)]
pub enum PatternKind {
    /// a regular expression relative to repository root, check [RegexMatcher]
    /// for supported RE syntax
    RE,

    /// a shell-style glob pattern relative to cwd
    Glob,

    /// a path relative to the repository root, and when the path points to a
    /// directory, it is matched recursively
    Path,

    /// an unrooted glob (e.g.: *.c matches C files in all dirs)
    RelGlob,

    /// a path relative to cwd
    RelPath,

    /// an unrooted regular expression, needn't match the start of a path
    RelRE,

    /// read file patterns per line from a file
    ListFile,

    /// read file patterns with null byte delimiters from a file
    ListFile0,

    /// a fileset expression
    Set,

    /// a path relative to repository root, which is matched non-recursively (will
    /// not match subdirectories)
    RootFilesIn,
}

impl PatternKind {
    pub fn name(&self) -> &'static str {
        match self {
            PatternKind::RE => "re",
            PatternKind::Glob => "glob",
            PatternKind::Path => "path",
            PatternKind::RelGlob => "relglob",
            PatternKind::RelPath => "relpath",
            PatternKind::RelRE => "relre",
            PatternKind::ListFile => "listfile",
            PatternKind::ListFile0 => "listfile0",
            PatternKind::Set => "set",
            PatternKind::RootFilesIn => "rootfilesin",
        }
    }

    pub fn is_glob(&self) -> bool {
        matches!(self, Self::Glob | Self::RelGlob)
    }

    pub fn is_path(&self) -> bool {
        matches!(self, Self::Path | Self::RelPath | Self::RootFilesIn)
    }

    pub fn is_regex(&self) -> bool {
        matches!(self, Self::RE | Self::RelRE)
    }

    pub fn is_recursive(&self) -> bool {
        matches!(self, Self::Path | Self::RelPath)
    }

    pub fn is_cwd_relative(&self) -> bool {
        matches!(self, Self::RelPath | Self::Glob)
    }

    pub fn is_free(&self) -> bool {
        matches!(self, Self::RelGlob | Self::RelRE)
    }
}

impl std::str::FromStr for PatternKind {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "re" => Ok(PatternKind::RE),
            "glob" => Ok(PatternKind::Glob),
            "path" => Ok(PatternKind::Path),
            "relglob" => Ok(PatternKind::RelGlob),
            "relpath" => Ok(PatternKind::RelPath),
            "relre" => Ok(PatternKind::RelRE),
            "listfile" => Ok(PatternKind::ListFile),
            "listfile0" => Ok(PatternKind::ListFile0),
            "set" => Ok(PatternKind::Set),
            "rootfilesin" => Ok(PatternKind::RootFilesIn),
            _ => Err(Error::UnsupportedPatternKind(s.to_string())),
        }
    }
}

#[derive(Debug, PartialEq)]
pub struct Pattern {
    pub(crate) kind: PatternKind,
    pub(crate) pattern: String,
    pub(crate) source: Option<String>,
}

impl Pattern {
    pub(crate) fn new(kind: PatternKind, pattern: String) -> Self {
        Self {
            kind,
            pattern,
            source: None,
        }
    }

    pub(crate) fn with_source(mut self, source: String) -> Self {
        self.source = Some(source);
        self
    }

    /// Build `Pattern` from str.
    ///
    /// * If the str doesn't have pattern kind prefix, we will use `default_kind`.
    /// * `source` is set to None.
    pub(crate) fn from_str(pattern: &str, default_kind: PatternKind) -> Self {
        let (kind, pat) = split_pattern(pattern, default_kind);
        Self {
            kind,
            pattern: pat.to_string(),
            source: None,
        }
    }
}

/// Build `Pattern`s from strings. It calls `Pattern::from_str` to do actual work.
pub fn build_patterns(patterns: &[String], default_kind: PatternKind) -> Vec<Pattern> {
    patterns
        .iter()
        .map(|s| Pattern::from_str(s, default_kind))
        .collect()
}

pub fn split_pattern<'a>(pattern: &'a str, default_kind: PatternKind) -> (PatternKind, &'a str) {
    match pattern.split_once(':') {
        Some((k, p)) => {
            if let Ok(kind) = PatternKind::from_str(k) {
                (kind, p)
            } else {
                (default_kind, pattern)
            }
        }
        None => (default_kind, pattern),
    }
}

#[tracing::instrument(level = "debug", ret)]
pub(crate) fn normalize_patterns<I>(
    patterns: I,
    default_kind: PatternKind,
    root: &Path,
    cwd: &Path,
    force_recursive_glob: bool,
) -> Result<Vec<Pattern>>
where
    I: IntoIterator + std::fmt::Debug,
    I::Item: AsRef<str> + std::fmt::Debug,
{
    let mut result = Vec::new();

    for pattern in patterns {
        let (kind, pat) = split_pattern(pattern.as_ref(), default_kind);

        // Expand curlies in globs (e.g. "foo/{bar/baz, qux}" to ["foo/bar/baz", "foo/qux"]).
        // Do this early so they aren't naively treated as paths. Note that we haven't
        // normalized "\" to "/", so a Windows path separator might be misinterpreted as a
        // curly escape. The alternative is to normalize "\" to "/" first, but that will
        // certainly break curly escapes.
        let pats = if kind.is_glob() {
            expand_curly_brackets(pat)
        } else {
            vec![pat.to_string()]
        };

        for mut pat in pats {
            // Normalize CWD-relative patterns to be relative to repo root.
            if kind.is_cwd_relative() {
                pat = match util::path::root_relative_path(root, cwd, pat.as_ref())? {
                    Some(pat) => pat
                        .into_os_string()
                        .into_string()
                        .map_err(|s| Error::NonUtf8(s.to_string_lossy().to_string()))?,
                    None => {
                        return Err(Error::PathOutsideRoot(
                            pat.to_string(),
                            root.to_string_lossy().to_string(),
                        )
                        .into());
                    }
                };
            }

            // Clean up path and normalize to "/" path separator.
            if kind.is_glob() || kind.is_path() {
                pat = normalize_path_pattern(&pat);

                // Path normalization yields "." for empty paths, which we don't want.
                if pat == "." {
                    pat = String::new();
                }
            }

            // Escape glob characters so we can convert non-glob patterns into globs.
            if kind.is_path() {
                pat = plain_to_glob(&pat);
            }

            // Make our loose globbing compatible with the tree matcher's strict globbing.
            if kind.is_glob() {
                pat = normalize_glob(&pat);
            }

            if kind.is_recursive() {
                pat = make_glob_recursive(&pat);
            }

            // This is to make "-I" and "-X" globs recursive by default.
            if force_recursive_glob && kind.is_glob() {
                pat = make_glob_recursive(&pat);
            }

            if kind.is_glob() && kind.is_free() {
                if !pat.is_empty() {
                    // relglob is unrooted, so give it a leading "**".
                    pat = format!("**/{pat}");
                }
            }

            // rootfilesin matches a directory non-recursively
            if kind == PatternKind::RootFilesIn {
                pat = if pat.is_empty() {
                    "*".to_string()
                } else {
                    format!("{pat}/*")
                };
            }

            if kind.is_regex() {
                let anchored = pat.starts_with('^');

                // "^" is not required - strip it.
                if anchored {
                    pat = pat[1..].to_string();
                }

                // relre without "^" needs leading ".*?" to become unanchored.
                if !anchored && kind.is_free() {
                    pat = format!(".*?{pat}");
                }
            }

            if kind.is_glob() || kind.is_path() || kind.is_regex() {
                result.push(Pattern::new(kind, pat));
            } else if matches!(kind, PatternKind::ListFile | PatternKind::ListFile0) {
                let contents = util::file::read_to_string(&pat)?;

                let patterns = if kind == PatternKind::ListFile {
                    normalize_patterns(contents.lines(), default_kind, root, cwd, false)?
                } else {
                    normalize_patterns(contents.split('\0'), default_kind, root, cwd, false)?
                };
                for p in patterns {
                    result.push(p.with_source(pat.clone()));
                }
            } else {
                return Err(Error::UnsupportedPatternKind(kind.name().to_string()).into());
            }
        }
    }

    Ok(result)
}

/// A wrapper of `util::path::normalize` function by adding path separator conversion,
/// yields normalized [String] if the pattern is valid unicode.
///
/// This function normalize the path difference on Windows by converting
/// path separator from `\` to `/`. This is need because our `RepoPathBuf`
/// is a path separated by `/`.
fn normalize_path_pattern(pattern: &str) -> String {
    let pattern = util::path::normalize(pattern.as_ref());
    // SAFTEY: In Rust, values of type String are always valid UTF-8.
    // Our input pattern is a &str, and we don't add invalid chars in
    // out `util::path::normalize` function, so it should be safe here.
    let pattern = pattern.into_os_string().into_string().unwrap();
    if cfg!(windows) {
        pattern.replace(
            std::path::MAIN_SEPARATOR,
            &types::path::SEPARATOR.to_string(),
        )
    } else {
        pattern
    }
}

#[cfg(test)]
mod tests {

    use std::fs;

    use tempfile::TempDir;

    use super::*;

    #[test]
    fn test_split_pattern() {
        let v = split_pattern("re:a.*py", PatternKind::Glob);
        assert_eq!(v, (PatternKind::RE, "a.*py"));

        let v = split_pattern("badkind:a.*py", PatternKind::Glob);
        assert_eq!(v, (PatternKind::Glob, "badkind:a.*py"));

        let v = split_pattern("a.*py", PatternKind::RE);
        assert_eq!(v, (PatternKind::RE, "a.*py"));
    }

    #[test]
    fn test_pattern_kind_enum() {
        assert_eq!(PatternKind::from_str("re").unwrap(), PatternKind::RE);
        assert!(PatternKind::from_str("invalid").is_err());

        assert_eq!(PatternKind::RE.name(), "re");
    }

    #[test]
    fn test_normalize_path_pattern() {
        assert_eq!(
            normalize_path_pattern("foo/bar/../baz/"),
            "foo/baz".to_string()
        );
    }

    #[track_caller]
    fn assert_normalize(pat: &str, expected: &[&str], root: &str, cwd: &str, recursive: bool) {
        let kind = pat.split_once(':').unwrap().0;

        let got: Vec<String> = normalize_patterns(
            vec![pat],
            PatternKind::Glob,
            root.as_ref(),
            cwd.as_ref(),
            recursive,
        )
        .unwrap()
        .into_iter()
        .map(|p| {
            assert_eq!(p.kind.name(), kind);
            p.pattern
        })
        .collect();

        assert_eq!(got, expected);
    }

    #[test]
    fn test_normalize_patterns() {
        #[track_caller]
        fn check(pat: &str, expected: &[&str]) {
            assert_normalize(pat, expected, "/root", "/root/cwd", false);
        }

        check("glob:", &["cwd"]);
        check("glob:.", &["cwd"]);
        check("glob:..", &[""]);
        check("glob:a", &["cwd/a"]);
        check("glob:../a{b,c}d", &["abd", "acd"]);
        check("glob:/root/foo/*.c", &["foo/*.c"]);

        check("relglob:", &[""]);
        check("relglob:.", &[""]);
        check("relglob:*.c", &["**/*.c"]);

        check("path:", &["**"]);
        check("path:.", &["**"]);
        check("path:foo", &["foo/**"]);
        check("path:foo*", &[r"foo\*/**"]);

        check("relpath:", &["cwd/**"]);
        check("relpath:.", &["cwd/**"]);
        check("relpath:foo", &["cwd/foo/**"]);
        check("relpath:../foo*", &[r"foo\*/**"]);

        check(r"re:a.*\.py", &[r"a.*\.py"]);

        check(r"relre:a.*\.py", &[r".*?a.*\.py"]);
        check(r"relre:^foo(bar|baz)", &[r"foo(bar|baz)"]);

        check("rootfilesin:", &["*"]);
        check("rootfilesin:.", &["*"]);
        check("rootfilesin:foo*", &[r"foo\*/*"]);
    }

    #[test]
    fn test_normalize_multiple() {
        let got: Vec<(PatternKind, String)> = normalize_patterns(
            vec!["naked", "relpath:foo/b{a}r", "glob:a{b,c}"],
            PatternKind::RelGlob,
            "/root".as_ref(),
            "/root/cwd".as_ref(),
            false,
        )
        .unwrap()
        .into_iter()
        .map(|p| (p.kind, p.pattern))
        .collect();

        assert_eq!(
            got,
            vec![
                (PatternKind::RelGlob, "**/naked".to_string()),
                (PatternKind::RelPath, "cwd/foo/b\\{a\\}r/**".to_string()),
                (PatternKind::Glob, "cwd/ab".to_string()),
                (PatternKind::Glob, "cwd/ac".to_string()),
            ]
        );
    }

    #[test]
    fn test_recursive_normalize() {
        #[track_caller]
        fn check(pat: &str, expected: &[&str]) {
            assert_normalize(pat, expected, "/root", "/root/cwd", true);
        }

        check("glob:", &["cwd/**"]);
        check("glob:/root", &["**"]);
    }

    #[test]
    fn test_normalize_patterns_unsupported_kind() {
        assert!(
            normalize_patterns(
                vec!["set:added()"],
                PatternKind::Glob,
                "".as_ref(),
                "".as_ref(),
                false,
            )
            .is_err()
        );
    }

    #[test]
    fn test_build_patterns() {
        let patterns = ["re:a.py".to_string(), "a.txt".to_string()];

        assert_eq!(
            build_patterns(&patterns, PatternKind::Glob),
            [
                Pattern::new(PatternKind::RE, "a.py".to_string()),
                Pattern::new(PatternKind::Glob, "a.txt".to_string())
            ]
        )
    }

    #[test]
    fn test_normalize_patterns_listfile() {
        test_normalize_patterns_listfile_helper("\n");
        test_normalize_patterns_listfile_helper("\r\n");
    }

    #[test]
    fn test_normalize_patterns_listfile0() {
        test_normalize_patterns_listfile_helper("\0");
    }

    fn test_normalize_patterns_listfile_helper(sep: &str) {
        let inner_patterns = vec!["glob:/a/*", r"re:a.*\.py"];
        let dir = TempDir::new().unwrap();
        let path = dir.path().join("patterns.txt");
        let path_str = path.to_string_lossy();
        let content = inner_patterns.join(sep);
        fs::write(&path, content).unwrap();

        let outer_patterns = vec![format!(
            "listfile{}:{}",
            if sep == "\0" { "0" } else { "" },
            path_str
        )];
        let result = normalize_patterns(
            outer_patterns,
            PatternKind::Glob,
            "".as_ref(),
            "".as_ref(),
            false,
        )
        .unwrap();

        assert_eq!(
            result,
            [
                Pattern::new(PatternKind::Glob, "/a/*".to_string())
                    .with_source(path_str.to_string()),
                Pattern::new(PatternKind::RE, r"a.*\.py".to_string())
                    .with_source(path_str.to_string())
            ]
        )
    }
}
