//===--- Parser.cpp - Matcher expression parser -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Recursive parser implementation for the matcher expression grammar.
///
//===----------------------------------------------------------------------===//

#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "clang/ASTMatchers/Dynamic/Registry.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/Twine.h"
#include <string>
#include <vector>

namespace clang {
namespace ast_matchers {
namespace dynamic {

/// \brief Simple structure to hold information for one token from the parser.
struct Parser::TokenInfo {
  /// \brief Different possible tokens.
  enum TokenKind {
    TK_Eof,
    TK_OpenParen,
    TK_CloseParen,
    TK_Comma,
    TK_Period,
    TK_Literal,
    TK_Ident,
    TK_InvalidChar,
    TK_Error,
    TK_CodeCompletion
  };

  /// \brief Some known identifiers.
  static const char* const ID_Bind;

  TokenInfo() : Text(), Kind(TK_Eof), Range(), Value() {}

  StringRef Text;
  TokenKind Kind;
  SourceRange Range;
  VariantValue Value;
};

const char* const Parser::TokenInfo::ID_Bind = "bind";

/// \brief Simple tokenizer for the parser.
class Parser::CodeTokenizer {
public:
  explicit CodeTokenizer(StringRef MatcherCode, Diagnostics *Error)
      : Code(MatcherCode), StartOfLine(MatcherCode), Line(1), Error(Error),
        CodeCompletionLocation(0) {
    NextToken = getNextToken();
  }

  CodeTokenizer(StringRef MatcherCode, Diagnostics *Error,
                unsigned CodeCompletionOffset)
      : Code(MatcherCode), StartOfLine(MatcherCode), Line(1), Error(Error),
        CodeCompletionLocation(MatcherCode.data() + CodeCompletionOffset) {
    NextToken = getNextToken();
  }

  /// \brief Returns but doesn't consume the next token.
  const TokenInfo &peekNextToken() const { return NextToken; }

  /// \brief Consumes and returns the next token.
  TokenInfo consumeNextToken() {
    TokenInfo ThisToken = NextToken;
    NextToken = getNextToken();
    return ThisToken;
  }

  TokenInfo::TokenKind nextTokenKind() const { return NextToken.Kind; }

private:
  TokenInfo getNextToken() {
    consumeWhitespace();
    TokenInfo Result;
    Result.Range.Start = currentLocation();

    if (CodeCompletionLocation && CodeCompletionLocation <= Code.data()) {
      Result.Kind = TokenInfo::TK_CodeCompletion;
      Result.Text = StringRef(CodeCompletionLocation, 0);
      CodeCompletionLocation = 0;
      return Result;
    }

    if (Code.empty()) {
      Result.Kind = TokenInfo::TK_Eof;
      Result.Text = "";
      return Result;
    }

    switch (Code[0]) {
    case ',':
      Result.Kind = TokenInfo::TK_Comma;
      Result.Text = Code.substr(0, 1);
      Code = Code.drop_front();
      break;
    case '.':
      Result.Kind = TokenInfo::TK_Period;
      Result.Text = Code.substr(0, 1);
      Code = Code.drop_front();
      break;
    case '(':
      Result.Kind = TokenInfo::TK_OpenParen;
      Result.Text = Code.substr(0, 1);
      Code = Code.drop_front();
      break;
    case ')':
      Result.Kind = TokenInfo::TK_CloseParen;
      Result.Text = Code.substr(0, 1);
      Code = Code.drop_front();
      break;

    case '"':
    case '\'':
      // Parse a string literal.
      consumeStringLiteral(&Result);
      break;

    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
      // Parse an unsigned literal.
      consumeUnsignedLiteral(&Result);
      break;

    default:
      if (isAlphanumeric(Code[0])) {
        // Parse an identifier
        size_t TokenLength = 1;
        while (1) {
          // A code completion location in/immediately after an identifier will
          // cause the portion of the identifier before the code completion
          // location to become a code completion token.
          if (CodeCompletionLocation == Code.data() + TokenLength) {
            CodeCompletionLocation = 0;
            Result.Kind = TokenInfo::TK_CodeCompletion;
            Result.Text = Code.substr(0, TokenLength);
            Code = Code.drop_front(TokenLength);
            return Result;
          }
          if (TokenLength == Code.size() || !isAlphanumeric(Code[TokenLength]))
            break;
          ++TokenLength;
        }
        Result.Kind = TokenInfo::TK_Ident;
        Result.Text = Code.substr(0, TokenLength);
        Code = Code.drop_front(TokenLength);
      } else {
        Result.Kind = TokenInfo::TK_InvalidChar;
        Result.Text = Code.substr(0, 1);
        Code = Code.drop_front(1);
      }
      break;
    }

    Result.Range.End = currentLocation();
    return Result;
  }

  /// \brief Consume an unsigned literal.
  void consumeUnsignedLiteral(TokenInfo *Result) {
    unsigned Length = 1;
    if (Code.size() > 1) {
      // Consume the 'x' or 'b' radix modifier, if present.
      switch (toLowercase(Code[1])) {
      case 'x': case 'b': Length = 2;
      }
    }
    while (Length < Code.size() && isHexDigit(Code[Length]))
      ++Length;

    Result->Text = Code.substr(0, Length);
    Code = Code.drop_front(Length);

    unsigned Value;
    if (!Result->Text.getAsInteger(0, Value)) {
      Result->Kind = TokenInfo::TK_Literal;
      Result->Value = Value;
    } else {
      SourceRange Range;
      Range.Start = Result->Range.Start;
      Range.End = currentLocation();
      Error->addError(Range, Error->ET_ParserUnsignedError) << Result->Text;
      Result->Kind = TokenInfo::TK_Error;
    }
  }

  /// \brief Consume a string literal.
  ///
  /// \c Code must be positioned at the start of the literal (the opening
  /// quote). Consumed until it finds the same closing quote character.
  void consumeStringLiteral(TokenInfo *Result) {
    bool InEscape = false;
    const char Marker = Code[0];
    for (size_t Length = 1, Size = Code.size(); Length != Size; ++Length) {
      if (InEscape) {
        InEscape = false;
        continue;
      }
      if (Code[Length] == '\\') {
        InEscape = true;
        continue;
      }
      if (Code[Length] == Marker) {
        Result->Kind = TokenInfo::TK_Literal;
        Result->Text = Code.substr(0, Length + 1);
        Result->Value = Code.substr(1, Length - 1).str();
        Code = Code.drop_front(Length + 1);
        return;
      }
    }

    StringRef ErrorText = Code;
    Code = Code.drop_front(Code.size());
    SourceRange Range;
    Range.Start = Result->Range.Start;
    Range.End = currentLocation();
    Error->addError(Range, Error->ET_ParserStringError) << ErrorText;
    Result->Kind = TokenInfo::TK_Error;
  }

  /// \brief Consume all leading whitespace from \c Code.
  void consumeWhitespace() {
    while (!Code.empty() && isWhitespace(Code[0])) {
      if (Code[0] == '\n') {
        ++Line;
        StartOfLine = Code.drop_front();
      }
      Code = Code.drop_front();
    }
  }

  SourceLocation currentLocation() {
    SourceLocation Location;
    Location.Line = Line;
    Location.Column = Code.data() - StartOfLine.data() + 1;
    return Location;
  }

  StringRef Code;
  StringRef StartOfLine;
  unsigned Line;
  Diagnostics *Error;
  TokenInfo NextToken;
  const char *CodeCompletionLocation;
};

Parser::Sema::~Sema() {}

struct Parser::ScopedContextEntry {
  Parser *P;

  ScopedContextEntry(Parser *P, MatcherCtor C) : P(P) {
    P->ContextStack.push_back(std::make_pair(C, 0u));
  }

  ~ScopedContextEntry() {
    P->ContextStack.pop_back();
  }

  void nextArg() {
    ++P->ContextStack.back().second;
  }
};

/// \brief Parse and validate a matcher expression.
/// \return \c true on success, in which case \c Value has the matcher parsed.
///   If the input is malformed, or some argument has an error, it
///   returns \c false.
bool Parser::parseMatcherExpressionImpl(VariantValue *Value) {
  const TokenInfo NameToken = Tokenizer->consumeNextToken();
  assert(NameToken.Kind == TokenInfo::TK_Ident);
  const TokenInfo OpenToken = Tokenizer->consumeNextToken();
  if (OpenToken.Kind != TokenInfo::TK_OpenParen) {
    Error->addError(OpenToken.Range, Error->ET_ParserNoOpenParen)
        << OpenToken.Text;
    return false;
  }

  llvm::Optional<MatcherCtor> Ctor =
      S->lookupMatcherCtor(NameToken.Text, NameToken.Range, Error);
  std::vector<ParserValue> Args;
  TokenInfo EndToken;

  {
    ScopedContextEntry SCE(this, Ctor ? *Ctor : 0);

    while (Tokenizer->nextTokenKind() != TokenInfo::TK_Eof) {
      if (Tokenizer->nextTokenKind() == TokenInfo::TK_CloseParen) {
        // End of args.
        EndToken = Tokenizer->consumeNextToken();
        break;
      }
      if (Args.size() > 0) {
        // We must find a , token to continue.
        const TokenInfo CommaToken = Tokenizer->consumeNextToken();
        if (CommaToken.Kind != TokenInfo::TK_Comma) {
          Error->addError(CommaToken.Range, Error->ET_ParserNoComma)
              << CommaToken.Text;
          return false;
        }
      }

      Diagnostics::Context Ctx(Diagnostics::Context::MatcherArg, Error,
                               NameToken.Text, NameToken.Range,
                               Args.size() + 1);
      ParserValue ArgValue;
      ArgValue.Text = Tokenizer->peekNextToken().Text;
      ArgValue.Range = Tokenizer->peekNextToken().Range;
      if (!parseExpressionImpl(&ArgValue.Value)) {
        return false;
      }

      Args.push_back(ArgValue);
      SCE.nextArg();
    }
  }

  if (EndToken.Kind == TokenInfo::TK_Eof) {
    Error->addError(OpenToken.Range, Error->ET_ParserNoCloseParen);
    return false;
  }

  std::string BindID;
  if (Tokenizer->peekNextToken().Kind == TokenInfo::TK_Period) {
    // Parse .bind("foo")
    Tokenizer->consumeNextToken();  // consume the period.
    const TokenInfo BindToken = Tokenizer->consumeNextToken();
    if (BindToken.Kind == TokenInfo::TK_CodeCompletion) {
      addCompletion(BindToken, "bind(\"", "bind");
      return false;
    }

    const TokenInfo OpenToken = Tokenizer->consumeNextToken();
    const TokenInfo IDToken = Tokenizer->consumeNextToken();
    const TokenInfo CloseToken = Tokenizer->consumeNextToken();

    // TODO: We could use different error codes for each/some to be more
    //       explicit about the syntax error.
    if (BindToken.Kind != TokenInfo::TK_Ident ||
        BindToken.Text != TokenInfo::ID_Bind) {
      Error->addError(BindToken.Range, Error->ET_ParserMalformedBindExpr);
      return false;
    }
    if (OpenToken.Kind != TokenInfo::TK_OpenParen) {
      Error->addError(OpenToken.Range, Error->ET_ParserMalformedBindExpr);
      return false;
    }
    if (IDToken.Kind != TokenInfo::TK_Literal || !IDToken.Value.isString()) {
      Error->addError(IDToken.Range, Error->ET_ParserMalformedBindExpr);
      return false;
    }
    if (CloseToken.Kind != TokenInfo::TK_CloseParen) {
      Error->addError(CloseToken.Range, Error->ET_ParserMalformedBindExpr);
      return false;
    }
    BindID = IDToken.Value.getString();
  }

  if (!Ctor)
    return false;

  // Merge the start and end infos.
  Diagnostics::Context Ctx(Diagnostics::Context::ConstructMatcher, Error,
                           NameToken.Text, NameToken.Range);
  SourceRange MatcherRange = NameToken.Range;
  MatcherRange.End = EndToken.Range.End;
  VariantMatcher Result = S->actOnMatcherExpression(
      *Ctor, MatcherRange, BindID, Args, Error);
  if (Result.isNull()) return false;

  *Value = Result;
  return true;
}

// If the prefix of this completion matches the completion token, add it to
// Completions minus the prefix.
void Parser::addCompletion(const TokenInfo &CompToken, StringRef TypedText,
                           StringRef Decl) {
  if (TypedText.size() >= CompToken.Text.size() &&
      TypedText.substr(0, CompToken.Text.size()) == CompToken.Text) {
    Completions.push_back(
        MatcherCompletion(TypedText.substr(CompToken.Text.size()), Decl));
  }
}

void Parser::addExpressionCompletions() {
  const TokenInfo CompToken = Tokenizer->consumeNextToken();
  assert(CompToken.Kind == TokenInfo::TK_CodeCompletion);

  // We cannot complete code if there is an invalid element on the context
  // stack.
  for (ContextStackTy::iterator I = ContextStack.begin(),
                                E = ContextStack.end();
       I != E; ++I) {
    if (!I->first)
      return;
  }

  std::vector<MatcherCompletion> RegCompletions =
      Registry::getCompletions(ContextStack);
  for (std::vector<MatcherCompletion>::iterator I = RegCompletions.begin(),
                                                E = RegCompletions.end();
       I != E; ++I) {
    addCompletion(CompToken, I->TypedText, I->MatcherDecl);
  }
}

/// \brief Parse an <Expresssion>
bool Parser::parseExpressionImpl(VariantValue *Value) {
  switch (Tokenizer->nextTokenKind()) {
  case TokenInfo::TK_Literal:
    *Value = Tokenizer->consumeNextToken().Value;
    return true;

  case TokenInfo::TK_Ident: {
    // Identifier could be a name known by Sema as a named value.
    const VariantValue NamedValue =
        S->getNamedValue(Tokenizer->peekNextToken().Text);
    if (!NamedValue.isNothing()) {
      Tokenizer->consumeNextToken();  // Actually consume it.
      *Value = NamedValue;
      return true;
    }
    // Fallback to full matcher parsing.
    return parseMatcherExpressionImpl(Value);
  }

  case TokenInfo::TK_CodeCompletion:
    addExpressionCompletions();
    return false;

  case TokenInfo::TK_Eof:
    Error->addError(Tokenizer->consumeNextToken().Range,
                    Error->ET_ParserNoCode);
    return false;

  case TokenInfo::TK_Error:
    // This error was already reported by the tokenizer.
    return false;

  case TokenInfo::TK_OpenParen:
  case TokenInfo::TK_CloseParen:
  case TokenInfo::TK_Comma:
  case TokenInfo::TK_Period:
  case TokenInfo::TK_InvalidChar:
    const TokenInfo Token = Tokenizer->consumeNextToken();
    Error->addError(Token.Range, Error->ET_ParserInvalidToken) << Token.Text;
    return false;
  }

  llvm_unreachable("Unknown token kind.");
}

Parser::Parser(CodeTokenizer *Tokenizer, Sema *S,
               Diagnostics *Error)
    : Tokenizer(Tokenizer), S(S), Error(Error) {}

Parser::RegistrySema::~RegistrySema() {}

llvm::Optional<MatcherCtor> Parser::RegistrySema::lookupMatcherCtor(
    StringRef MatcherName, const SourceRange &NameRange, Diagnostics *Error) {
  return Registry::lookupMatcherCtor(MatcherName, NameRange, Error);
}

VariantMatcher Parser::RegistrySema::actOnMatcherExpression(
    MatcherCtor Ctor, const SourceRange &NameRange, StringRef BindID,
    ArrayRef<ParserValue> Args, Diagnostics *Error) {
  if (BindID.empty()) {
    return Registry::constructMatcher(Ctor, NameRange, Args, Error);
  } else {
    return Registry::constructBoundMatcher(Ctor, NameRange, BindID, Args,
                                           Error);
  }
}

bool Parser::parseExpression(StringRef Code, VariantValue *Value,
                             Diagnostics *Error) {
  RegistrySema S;
  return parseExpression(Code, &S, Value, Error);
}

bool Parser::parseExpression(StringRef Code, Sema *S,
                             VariantValue *Value, Diagnostics *Error) {
  CodeTokenizer Tokenizer(Code, Error);
  if (!Parser(&Tokenizer, S, Error).parseExpressionImpl(Value)) return false;
  if (Tokenizer.peekNextToken().Kind != TokenInfo::TK_Eof) {
    Error->addError(Tokenizer.peekNextToken().Range,
                    Error->ET_ParserTrailingCode);
    return false;
  }
  return true;
}

std::vector<MatcherCompletion>
Parser::completeExpression(StringRef Code, unsigned CompletionOffset) {
  Diagnostics Error;
  CodeTokenizer Tokenizer(Code, &Error, CompletionOffset);
  RegistrySema S;
  Parser P(&Tokenizer, &S, &Error);
  VariantValue Dummy;
  P.parseExpressionImpl(&Dummy);

  return P.Completions;
}

llvm::Optional<DynTypedMatcher>
Parser::parseMatcherExpression(StringRef Code, Diagnostics *Error) {
  RegistrySema S;
  return parseMatcherExpression(Code, &S, Error);
}

llvm::Optional<DynTypedMatcher>
Parser::parseMatcherExpression(StringRef Code, Parser::Sema *S,
                               Diagnostics *Error) {
  VariantValue Value;
  if (!parseExpression(Code, S, &Value, Error))
    return llvm::Optional<DynTypedMatcher>();
  if (!Value.isMatcher()) {
    Error->addError(SourceRange(), Error->ET_ParserNotAMatcher);
    return llvm::Optional<DynTypedMatcher>();
  }
  llvm::Optional<DynTypedMatcher> Result =
      Value.getMatcher().getSingleMatcher();
  if (!Result.hasValue()) {
    Error->addError(SourceRange(), Error->ET_ParserOverloadedType)
        << Value.getTypeAsString();
  }
  return Result;
}

}  // namespace dynamic
}  // namespace ast_matchers
}  // namespace clang
