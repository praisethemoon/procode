//! A tiny tokenizer for arithmetic expressions.

use std::fmt;
use std::iter::Peekable;
use std::str::Chars;

/// One token of an expression.
#[derive(Debug, Clone, PartialEq)]
pub enum Token {
    Number(f64),
    Ident(String),
    Op(char),
    LParen,
    RParen,
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Token::Number(n) => write!(f, "{}", n),
            Token::Ident(s) => write!(f, "{}", s),
            Token::Op(c) => write!(f, "{}", c),
            Token::LParen => write!(f, "("),
            Token::RParen => write!(f, ")"),
        }
    }
}

/// Splits source text into tokens, skipping whitespace.
pub struct Lexer<'a> {
    chars: Peekable<Chars<'a>>,
}

impl<'a> Lexer<'a> {
    pub fn new(src: &'a str) -> Self {
        Lexer { chars: src.chars().peekable() }
    }

    fn number(&mut self, first: char) -> Token {
        let mut s = first.to_string();
        while let Some(&c) = self.chars.peek() {
            if c.is_ascii_digit() || c == '.' {
                s.push(c);
                self.chars.next();
            } else {
                break;
            }
        }
        Token::Number(s.parse().unwrap_or(0.0))
    }

    fn ident(&mut self, first: char) -> Token {
        let mut s = first.to_string();
        while let Some(&c) = self.chars.peek() {
            if c.is_alphanumeric() || c == '_' {
                s.push(c);
                self.chars.next();
            } else {
                break;
            }
        }
        Token::Ident(s)
    }
}

impl<'a> Iterator for Lexer<'a> {
    type Item = Token;

    fn next(&mut self) -> Option<Token> {
        while let Some(c) = self.chars.next() {
            return Some(match c {
                ' ' | '\t' | '\n' => continue,
                '0'..='9' => self.number(c),
                'a'..='z' | 'A'..='Z' | '_' => self.ident(c),
                '(' => Token::LParen,
                ')' => Token::RParen,
                _ => Token::Op(c),
            });
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splits_an_expression() {
        let t: Vec<Token> = Lexer::new("x + 2.5 * (y - 1)").collect();
        assert_eq!(t.len(), 9);
        assert_eq!(t[0], Token::Ident("x".into()));
    }
}
