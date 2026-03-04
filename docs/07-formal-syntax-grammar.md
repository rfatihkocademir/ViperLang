# ViperLang — Formal Syntax Grammar (EBNF)

> ViperLang'ın derleyici parser'ı tarafından kullanılacak, LLM uyumlu, ambiguiti'den (belirsizlikten) arındırılmış **Formal Dil Bilgisi (Grammar)** spesifikasyonudur.

---

## Lexical Structure (Sözcük Yapısı)

```ebnf
File            ::= ContractBlock? Statement*

ContractBlock   ::= "// @contract" Newline ContractLine*
ContractLine    ::= "// " ("export_st" | "export_fn" | "export_tr" | "uses") ":" [^\n]* Newline

Identifier      ::= [a-zA-Z_] [a-zA-Z0-9_]*
TypeName        ::= [A-Z] [a-zA-Z0-9_]* | "i" | "f" | "b" | "s" | "c" | "u8" | "any"
Keyword         ::= "v" | "c" | "fn" | "r" | "i" | "ei" | "e" | "f" | "w" | "l" | "m" | 
                    "st" | "en" | "tr" | "impl" | "use" | "tp" | "pub" | "spawn" | 
                    "async" | "await"

Comment         ::= "//" [^\n]* | "/*" .*? "*/" | "///" [^\n]*
```

## Types (Tipler)

```ebnf
Type            ::= PrimitiveType | GenericType | ArrayType | MapType | TupleType | NullableType | UnionType
PrimitiveType   ::= "i" | "f" | "b" | "s" | "c" | "u8" | "any"
GenericType     ::= TypeName ("<" Type ("," Type)* ">")?
ArrayType       ::= "[" Type "]" | "[" Type ";" IntLiteral "]"
MapType         ::= "{" Type ":" Type "}"
TupleType       ::= "(" Type ("," Type)+ ")"
NullableType    ::= Type "?"
UnionType       ::= Type "|" Type ("|" Type)*
```

## Declarations (Tanımlamalar)

```ebnf
Statement       ::= VarDecl | ConstDecl | FnDecl | StructDecl | EnumDecl | TraitDecl | 
                    ImplDecl | UseDecl | TypeAliasDecl | ExprStmt

VarDecl         ::= "v" Identifier (":" Type)? "=" Expression
ConstDecl       ::= "c" Identifier (":" Type)? "=" Expression

FnDecl          ::= Attribute* "pub"? "async"? "fn" Identifier 
                    GenericParams? "(" ParamList? ")" ReturnType? BlockOrExpr
GenericParams   ::= "<" Identifier ("," Identifier)* ">"
ParamList       ::= Param (Whitespace Param)*
Param           ::= "mut"? Identifier ":" Type ("=" Expression)?
ReturnType      ::= Type
BlockOrExpr     ::= Block | "=" Expression

UseDecl         ::= "use" Identifier ("." Identifier | ".{" Identifier (Whitespace Identifier)* "}")? 
                    ("->" Identifier)?

TypeAliasDecl   ::= "tp" Identifier "=" Type
```

## Data Structures (Veri Yapıları)

```ebnf
StructDecl      ::= Attribute* "st" Identifier GenericParams? "(" StructFields? ")" 
                    ("impl" TraitList)? StructBody?
StructFields    ::= StructField (Whitespace StructField)*
StructField     ::= Identifier ":" Type ("=" Expression)?
TraitList       ::= TypeName ("+" TypeName)*
StructBody      ::= "{" (ComputedField | FnDecl)* "}"
ComputedField   ::= Identifier ":" Type "=" Expression

EnumDecl        ::= Attribute* "en" Identifier GenericParams? "{" EnumVariant* "}"
EnumVariant     ::= Identifier ("(" StructFields ")")?

TraitDecl       ::= "tr" Identifier "{" TraitMethod* "}"
TraitMethod     ::= "fn" Identifier GenericParams? "(" ParamList? ")" ReturnType?
ImplDecl        ::= "impl" TypeName "for" TypeName "{" FnDecl* "}"

Attribute       ::= "@" Identifier ("(" ArgumentList ")")? | 
                    "#[" Identifier ("(" ArgumentList ")")? (Whitespace Identifier)* "]"
```

## Control Flow (Kontrol Akışı)

```ebnf
IfExpr          ::= "i" Condition BlockOrLabel 
                    ("ei" Condition BlockOrLabel)* 
                    ("e" BlockOrLabel)?
Condition       ::= Expression | LetBind
LetBind         ::= Pattern "=" Expression
BlockOrLabel    ::= Block | ":" Statement

MatchExpr       ::= "m" Expression "{" MatchArm+ "}"
MatchArm        ::= Pattern "=>" Expression
Pattern         ::= Literal | Identifier | Identifier "(" PatternList ")" | "_"
PatternList     ::= Pattern (Whitespace Pattern)*

ForExpr         ::= "f" Identifier ("," Identifier)? "in" Expression BlockOrLabel
WhileExpr       ::= "w" Condition BlockOrLabel
LoopExpr        ::= "l" BlockOrLabel
```

## Expressions (İfadeler)

```ebnf
Expression      ::= BinaryExpr | UnaryExpr | PrimaryExpr | PipeExpr | MatchExpr | IfExpr

PipeExpr        ::= Expression "|>" Expression ("|>" Expression)*
AssignmentExpr  ::= Expression ("=" | "+=" | "-=" | "*=" | "/=" | "%=") Expression

PrimaryExpr     ::= Literal | Identifier | FnCall | MethodCall | FieldAccess | 
                    ArrayLiteral | MapLiteral | TupleLiteral | Closure | Block

Literal         ::= IntLiteral | FloatLiteral | StringLiteral | "true" | "false" | "nil"

Closure         ::= "|" ParamList? "|" ReturnType? Expression
FnCall          ::= Identifier "(" ArgumentList? ")"
MethodCall      ::= Expression "." Identifier "(" ArgumentList? ")"
FieldAccess     ::= Expression ("." Identifier | "?." Identifier)

ArgumentList    ::= Expression (Whitespace Expression)*
```

## Yüksek Seviye Semantik Kurallar (LLM Garantileri)

1.  **Strict Typing (Katı Tipler):** Her değişken ve parametre bir tipe çözümlenmelidir. Derleme anında "unknown/any" (eğer açıkça belirtilmemişse) durumlarına izin verilmez.
2.  **No Shadowing (Gölgelenme Yok):** Aynı scope veya alt scope'da aynı isimde değişken tanımlanması derleme hatasıdır.
3.  **Exhaustive Match (Kapsamlı Eşleşme):** Enum'lar üzerindeki `m` (match) ifadeleri tüm olası varyantları (veya `_` default) içermelidir.
4.  **Implicitness via Predictability (Öngörülebilirlik Üzerinden Örtüklük):** `self` kelimesi yasaktır. Metod içindeki `.` erişimleri otomatik olarak o metoda ait struct'ı temsil eder. Derleyici bu kuralı istisnasız uygular.
