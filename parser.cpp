// This file is part of the IMP project.

#include <sstream>

#include "parser.h"
#include "lexer.h"
#include "ast.h"



// -----------------------------------------------------------------------------
static std::string FormatMessage(const Location &loc, const std::string &msg)
{
  std::ostringstream os;
  os << "[" << loc.Name << ":" << loc.Line << ":" << loc.Column << "] " << msg;
  return os.str();
}

// -----------------------------------------------------------------------------
ParserError::ParserError(const Location &loc, const std::string &msg)
  : std::runtime_error(FormatMessage(loc, msg))
{
}


// -----------------------------------------------------------------------------
Parser::Parser(Lexer &lexer)
  : lexer_(lexer)
{
}

// -----------------------------------------------------------------------------
std::shared_ptr<Module> Parser::ParseModule()
{
  std::vector<TopLevelStmt> body;
  while (auto tk = Current()) {
    if (tk.Is(Token::Kind::FUNC)) {
      // Parse a function prototype or declaration.
      std::string name(Expect(Token::Kind::IDENT).GetIdent());
      Expect(Token::Kind::LPAREN);

      std::vector<std::pair<std::string, std::string>> args;
      while (!lexer_.Next().Is(Token::Kind::RPAREN)) {
        std::string arg(Current().GetIdent());
        Expect(Token::Kind::COLON);
        std::string type(Expect(Token::Kind::IDENT).GetIdent());
        args.emplace_back(arg, type);

        if (!lexer_.Next().Is(Token::Kind::COMMA)) {
          break;
        }
      }
      Check(Token::Kind::RPAREN);

      Expect(Token::Kind::COLON);
      std::string type(Expect(Token::Kind::IDENT).GetIdent());

      if (lexer_.Next().Is(Token::Kind::EQUAL)) {
        std::string primitive(Expect(Token::Kind::STRING).GetString());
        lexer_.Next();
        body.push_back(std::make_shared<ProtoDecl>(
            name,
            std::move(args),
            type,
            primitive
        ));
      } else {
        auto block = ParseBlockStmt();
        body.push_back(std::make_shared<FuncDecl>(
            name,
            std::move(args),
            type,
            block
        ));
      }
    } else {
      // Parse a top-level statement.
      body.push_back(ParseStmt());
    }
  }
  return std::make_unique<Module>(std::move(body));
}

// -----------------------------------------------------------------------------
std::shared_ptr<Stmt> Parser::ParseStmt()
{
  auto tk = Current();
  switch (tk.GetKind()) {
    case Token::Kind::RETURN: return ParseReturnStmt();
    case Token::Kind::WHILE: return ParseWhileStmt();
    case Token::Kind::LBRACE: return ParseBlockStmt();
    case Token::Kind::IF: return ParseIfStmt();
    default: return std::make_shared<ExprStmt>(ParseExpr());
  }
}

// -----------------------------------------------------------------------------
std::shared_ptr<BlockStmt> Parser::ParseBlockStmt()
{
  Check(Token::Kind::LBRACE);

  std::vector<std::shared_ptr<Stmt>> body;
  while (!lexer_.Next().Is(Token::Kind::RBRACE)) {
    body.push_back(ParseStmt());  
    if (!Current().Is(Token::Kind::SEMI)) {
      break;      
    }
  }
  Check(Token::Kind::RBRACE);
  lexer_.Next();
  return std::make_shared<BlockStmt>(std::move(body));
}

// -----------------------------------------------------------------------------
std::shared_ptr<ReturnStmt> Parser::ParseReturnStmt()
{
  Check(Token::Kind::RETURN);
  lexer_.Next();
  auto expr = ParseExpr();
  return std::make_shared<ReturnStmt>(expr);
}

// -----------------------------------------------------------------------------
std::shared_ptr<WhileStmt> Parser::ParseWhileStmt()
{
  Check(Token::Kind::WHILE);
  Expect(Token::Kind::LPAREN);
  lexer_.Next();
  auto cond = ParseExpr();
  Check(Token::Kind::RPAREN);
  lexer_.Next();
  auto stmt = ParseStmt();
  return std::make_shared<WhileStmt>(cond, stmt);
}

// -----------------------------------------------------------------------------
std::shared_ptr<IfStmt> Parser::ParseIfStmt() {
  Check(Token::Kind::IF);
  Expect(Token::Kind::LPAREN);
  lexer_.Next();
  auto cond = ParseExpr();
  Check(Token::Kind::RPAREN);
  lexer_.Next();
  auto stmt = ParseStmt();

  if(Current().Is(Token::Kind::ELSE)) {
    lexer_.Next();
    auto elseStmt = ParseStmt();

    return std::make_shared<IfStmt>(cond, stmt, elseStmt);
  }

  return std::make_shared<IfStmt>(cond, stmt, nullptr);
}


// -----------------------------------------------------------------------------
std::shared_ptr<Expr> Parser::ParseTermExpr()
{
  auto tk = Current();
  switch (tk.GetKind()) {
    case Token::Kind::IDENT: {
      std::string ident(tk.GetIdent());
      lexer_.Next();
      return std::static_pointer_cast<Expr>(
          std::make_shared<RefExpr>(ident)
      );
    }
    case Token::Kind::INT: {
      std::uint64_t integer(tk.GetInt());
      lexer_.Next();
      return std::static_pointer_cast<Expr>(
        std::make_shared<IntExpr>(integer)
      );
    }
    case Token::Kind::LPAREN: { 
      lexer_.Next();
      auto expr = ParseExpr();
      Check(Token::Kind::RPAREN);
      lexer_.Next();
      return expr;
    }
    default: {
      std::ostringstream os;
      os << "unexpected " << tk << ", expecting term";
      Error(tk.GetLocation(), os.str());
    }
  }
}

// -----------------------------------------------------------------------------
std::shared_ptr<Expr> Parser::ParseCallExpr()
{
  std::shared_ptr<Expr> callee = ParseTermExpr();
  while (Current().Is(Token::Kind::LPAREN)) {
    std::vector<std::shared_ptr<Expr>> args;
    while (!lexer_.Next().Is(Token::Kind::RPAREN)) {
      args.push_back(ParseExpr());
      if (!Current().Is(Token::Kind::COMMA)) {
        break;
      }
    }
    Check(Token::Kind::RPAREN);
    lexer_.Next();
    callee = std::make_shared<CallExpr>(callee, std::move(args));
  }
  return callee;
}

// -----------------------------------------------------------------------------
std::shared_ptr<Expr> Parser::ParseMulDivExpr()
{
  std::shared_ptr<Expr> term = ParseCallExpr();
  while(Current().Is(Token::Kind::MULTIPLY) || Current().Is(Token::Kind::DIVIDE) || Current().Is(Token::Kind::MODULO)) {
    auto tk = Current();

    lexer_.Next();
    auto rhs = ParseCallExpr();

    term = std::make_shared<BinaryExpr>(
      tk.Is(Token::Kind::MULTIPLY) ?  BinaryExpr::Kind::MUL : (
                                      tk.Is(Token::Kind::DIVIDE) ?  BinaryExpr::Kind::DIV : 
                                                                    BinaryExpr::Kind::MOD), term, rhs);
  }

  return term;
}

// -----------------------------------------------------------------------------
std::shared_ptr<Expr> Parser::ParseAddSubExpr()
{
  std::shared_ptr<Expr> term = ParseMulDivExpr();
  while (Current().Is(Token::Kind::PLUS) || Current().Is(Token::Kind::MINUS)) {
    auto tk = Current();

    lexer_.Next();
    auto rhs = ParseMulDivExpr();

    term = std::make_shared<BinaryExpr>(tk.Is(Token::Kind::PLUS) ? BinaryExpr::Kind::ADD : BinaryExpr::Kind::SUB, term, rhs);
  }

  return term;
}

// -----------------------------------------------------------------------------
std::shared_ptr<Expr> Parser::ParseCompExpr() 
{
  std::shared_ptr<Expr> term = ParseAddSubExpr();
  while(Current().Is(Token::Kind::DOUBLE_EQUAL) ||
        Current().Is(Token::Kind::NOT_EQUAL) ||
        Current().Is(Token::Kind::SMALLER) ||
        Current().Is(Token::Kind::SMALLER_OR_EQUAL) ||
        Current().Is(Token::Kind::GREATER) ||
        Current().Is(Token::Kind::GREATER_OR_EQUAL)) {

    auto tk = Current();

    lexer_.Next();
    auto rhs = ParseAddSubExpr();

    term = std::make_shared<BinaryExpr>(tk.Is(Token::Kind::DOUBLE_EQUAL) ? BinaryExpr::Kind::DEQ : 
                                       (tk.Is(Token::Kind::NOT_EQUAL) ? BinaryExpr::Kind::NEQ :
                                       (tk.Is(Token::Kind::SMALLER) ? BinaryExpr::Kind::SM : 
                                       (tk.Is(Token::Kind::SMALLER_OR_EQUAL) ? BinaryExpr::Kind::SMEQ : 
                                       (tk.Is(Token::Kind::GREATER) ? BinaryExpr::Kind::GR : BinaryExpr::Kind::GREQ)))), term, rhs);
  }

  return term;
}

// -----------------------------------------------------------------------------
const Token &Parser::Expect(Token::Kind kind)
{
  lexer_.Next();
  return Check(kind);
}

// -----------------------------------------------------------------------------
const Token &Parser::Check(Token::Kind kind)
{
  const auto &tk = Current();
  if (kind != tk.GetKind()) {
    std::ostringstream os;
    os << "unexpected " << tk << ", expecting " << kind;
    Error(tk.GetLocation(), os.str());
  }
  return tk;
}

// -----------------------------------------------------------------------------
void Parser::Error(const Location &loc, const std::string &msg)
{
  throw ParserError(loc, msg);
}
