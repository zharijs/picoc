#include "picoc.h"


/* whether evaluation is left to right for a given precedence level */
#define IS_LEFT_TO_RIGHT(p) ((p) != 2 && (p) != 3 && (p) != 14)
#ifndef NO_FP
#define IS_INTEGER_COERCIBLE(v) ((v)->Typ->Base == TypeInt || (v)->Typ->Base == TypeFP || (v)->Typ->Base == TypeChar)
#define COERCE_INTEGER(v) (((v)->Typ->Base == TypeFP) ? (int)(v)->Val->FP : (v)->Val->Integer)
#else
#define IS_INTEGER_COERCIBLE(v) ((v)->Typ->Base == TypeInt || (v)->Typ->Base == TypeChar)
#define COERCE_INTEGER(v) ((v)->Val->Integer)
#endif

/* helper macros for assignment */
#define ASSIGN_INT(d,s) { if (!(d)->IsLValue) ProgramFail(Parser, "can't assign to this"); ResultInt = (s); (d)->Val->Integer = ResultInt; }
#define ASSIGN_INT_AFTER(d,s) { if (!(d)->IsLValue) ProgramFail(Parser, "can't assign to this"); ResultInt = (d)->Val->Integer; (d)->Val->Integer = (s); }
#define ASSIGN_FP(d,s) { if (!(d)->IsLValue) ProgramFail(Parser, "can't assign to this"); ResultFP = (s); (d)->Val->FP = ResultFP; }

#define BRACKET_PRECEDENCE 20

#ifdef DEBUG_EXPRESSIONS
#define debugf printf
#else
void debugf(char *Format, ...)
{
}
#endif

#ifndef NEW_EXPRESSION_SYSTEM
/* local prototypes */
void ExpressionParseFunctionCall(struct ParseState *Parser, struct Value **Result, const char *FuncName);

/* parse a single value */
int ExpressionParseValue(struct ParseState *Parser, struct Value **Result)
{
    struct ParseState PreState = *Parser;
    struct Value *LexValue;
    int IntValue;
    enum LexToken Token = LexGetToken(Parser, &LexValue, TRUE);
    struct Value *LocalLValue = NULL;
    struct ValueType *VType;
    char *Identifier;
    int Success = TRUE;
    
    switch (Token)
    {
        case TokenIntegerConstant: case TokenCharacterConstant: case TokenFPConstant: case TokenStringConstant:
            *Result = VariableAllocValueAndCopy(Parser, LexValue, FALSE);
            break;
        
        case TokenMinus:  case TokenUnaryExor: case TokenUnaryNot:
            IntValue = ExpressionParseInt(Parser);
            if (Parser->Mode == RunModeRun)
            {
                *Result = VariableAllocValueFromType(Parser, &IntType, FALSE, NULL);
                switch(Token)
                {
                    case TokenMinus: (*Result)->Val->Integer = -IntValue; break;
                    case TokenUnaryExor: (*Result)->Val->Integer = ~IntValue; break;
                    case TokenUnaryNot: (*Result)->Val->Integer = !IntValue; break;
                    default: break;
                }
            }
            break;

        case TokenOpenBracket:
            if (!ExpressionParse(Parser, Result))
                ProgramFail(Parser, "invalid expression");
            
            if (LexGetToken(Parser, NULL, TRUE) != TokenCloseBracket)
                ProgramFail(Parser, "')' expected");
            break;
                
        case TokenAsterisk:
            if (!ExpressionParse(Parser, Result))
                ProgramFail(Parser, "invalid expression");
            
            if ((*Result)->Typ->Base != TypePointer)
                ProgramFail(Parser, "can't dereference this non-pointer");
            
            LocalLValue = (*Result)->Val->Pointer.Segment;
            VariableStackPop(Parser, *Result);
            *Result = VariableAllocValueShared(Parser, LocalLValue);
            break;

        case TokenAmpersand:
            if (!ExpressionParseValue(Parser, Result) || !(*Result)->IsLValue)
                ProgramFail(Parser, "can't get the address of this");
            
            VType = (*Result)->Typ;
            LocalLValue = (*Result)->LValueFrom;
            VariableStackPop(Parser, *Result);
            *Result = VariableAllocValueFromType(Parser, TypeGetMatching(Parser, VType, TypePointer, 0, StrEmpty), FALSE, NULL);
            (*Result)->Val->Pointer.Segment = LocalLValue;
            (*Result)->Val->Pointer.Offset = (void *)(*Result)->Val - (void *)(*Result)->LValueFrom;
            break;
            
        case TokenIdentifier:
            if (LexGetToken(Parser, NULL, FALSE) == TokenOpenBracket)
                ExpressionParseFunctionCall(Parser, Result, LexValue->Val->Identifier);
            else
            {
                if (Parser->Mode == RunModeRun)
                {
                    VariableGet(Parser, LexValue->Val->Identifier, &LocalLValue);
                    if (LocalLValue->Typ->Base == TypeMacro)
                    {
                        struct ParseState MacroParser = LocalLValue->Val->Parser;
                        
                        if (!ExpressionParse(&MacroParser, Result) || LexGetToken(&MacroParser, NULL, FALSE) != TokenEndOfFunction)
                            ProgramFail(&MacroParser, "expression expected");
                    }
                    else if (LocalLValue->Typ == TypeVoid)
                        ProgramFail(Parser, "a void value isn't much use here");
                    else
                    { /* it's a value variable */
                        *Result = VariableAllocValueShared(Parser, LocalLValue);
                    }
                }
                
                /* see if there's a postfix operator */
                Token = LexGetToken(Parser, &LexValue, FALSE);
                if (Token == TokenIncrement || Token == TokenDecrement)
                { /* it's a postincrement or postdecrement */
                    LexGetToken(Parser, &LexValue, TRUE);
                    if (Parser->Mode == RunModeRun)
                    {
                        if (!(*Result)->IsLValue || (*Result)->Typ->Base != TypeInt) 
                            ProgramFail(Parser, "can't %s this", (Token == TokenIncrement) ? "increment" : "decrement");
                            
                        if (Token == TokenIncrement)
                            (*Result)->Val->Integer++;
                        else
                            (*Result)->Val->Integer--;
                    }
                }
                else if (Token == TokenLeftSquareBracket)
                { /* array access */
                    LexGetToken(Parser, &LexValue, TRUE);
                    IntValue = ExpressionParseInt(Parser);
                    if (LexGetToken(Parser, &LexValue, TRUE) != TokenRightSquareBracket)
                        ProgramFail(Parser, "expected ']'");
                    
                    if (Parser->Mode == RunModeRun)
                    { /* look up the array element */
                        if ((*Result)->Typ->Base != TypeArray)
                            ProgramFail(Parser, "not an array");
                        
                        if (IntValue < 0 || IntValue >= (*Result)->Val->Array.Size)
                            ProgramFail(Parser, "illegal array index");
                        
                        VariableStackPop(Parser, *Result);
                        *Result = VariableAllocValueFromExistingData(Parser, (*Result)->Typ->FromType, (union AnyValue *)((*Result)->Val->Array.Data + TypeSize((*Result)->Typ->FromType, 0) * IntValue), (*Result)->IsLValue, (*Result)->LValueFrom);
                    }
                }
            }
            break;

        case TokenNew:
            TypeParse(Parser, &VType, &Identifier);
            if (Identifier != StrEmpty)
                ProgramFail(Parser, "identifier not expected here");
            
            if (Parser->Mode == RunModeRun)
            { /* create an object of this type on the heap and a pointer to it on the stack */
                LocalLValue = VariableAllocValueFromType(Parser, VType, TRUE, NULL);
                *Result = VariableAllocValueFromType(Parser, TypeGetMatching(Parser, VType, TypePointer, 0, StrEmpty), FALSE, NULL);
                (*Result)->Val->Pointer.Segment = LocalLValue;
                (*Result)->Val->Pointer.Offset = 0;
            }
            break;
            
        default:
            *Parser = PreState;
            Success = FALSE;
            break;
    }
    
    return Success;
}

#ifndef NO_FP
struct Value *ParsePushFP(struct ParseState *Parser, double NewFP)
{
    struct Value *Val = VariableAllocValueFromType(Parser, &FPType, FALSE, NULL);
    Val->Val->FP = NewFP;
    return Val;
}
#endif

struct Value *ParsePushInt(struct ParseState *Parser, int NewInt)
{
    struct Value *Val = VariableAllocValueFromType(Parser, &IntType, FALSE, NULL);
    Val->Val->Integer = NewInt;
    return Val;
}

/* parse an expression. operator precedence is not supported */
int ExpressionParse(struct ParseState *Parser, struct Value **Result)
{
    struct Value *CurrentValue;
    struct Value *TotalValue;
    
    if (!ExpressionParseValue(Parser, &TotalValue))
        return FALSE;
    
    while (TRUE)
    {
        enum LexToken Token = LexGetToken(Parser, NULL, FALSE);
        switch (Token)
        {
            case TokenPlus: case TokenMinus: case TokenAsterisk: case TokenSlash:
            case TokenEqual: case TokenLessThan: case TokenGreaterThan:
            case TokenLessEqual: case TokenGreaterEqual: case TokenLogicalAnd:
            case TokenLogicalOr: case TokenAmpersand: case TokenArithmeticOr: 
            case TokenArithmeticExor: 
                LexGetToken(Parser, NULL, TRUE);
                break;
            
            case TokenDot:
            {
                struct Value *Ident;
                
                LexGetToken(Parser, NULL, TRUE);
                if (LexGetToken(Parser, &Ident, TRUE) != TokenIdentifier)
                    ProgramFail(Parser, "need an structure or union member after '.'");

                if (Parser->Mode == RunModeRun)
                {                
                    void *TotalValueData = (void *)TotalValue->Val;
                    struct Value *TotalLValueFrom = TotalValue->LValueFrom;

                    if (TotalValue->Typ->Base != TypeStruct && TotalValue->Typ->Base != TypeUnion)
                        ProgramFail(Parser, "can't use '.' on something that's not a struct or union");
                        
                    if (!TableGet(TotalValue->Typ->Members, Ident->Val->Identifier, &CurrentValue))
                        ProgramFail(Parser, "structure doesn't have a member called '%s'", Ident->Val->Identifier);
                    
                    VariableStackPop(Parser, TotalValue);
                    TotalValue = VariableAllocValueFromExistingData(Parser, CurrentValue->Typ, TotalValueData + CurrentValue->Val->Integer, TRUE, TotalLValueFrom);
                }
                continue;
            }
            case TokenArrow:
            {
                struct Value *Ident;
                
                LexGetToken(Parser, NULL, TRUE);
                if (LexGetToken(Parser, &Ident, TRUE) != TokenIdentifier)
                    ProgramFail(Parser, "need an structure or union member after '->'");

                if (Parser->Mode == RunModeRun)
                {                
                    void *TotalValueData;
                    struct Value *DerefValue;

                    if (TotalValue->Typ->Base != TypePointer)
                        ProgramFail(Parser, "can't dereference this non-pointer");
                    
                    DerefValue = TotalValue->Val->Pointer.Segment;
                    TotalValueData = (void *)DerefValue->Val;

                    if (DerefValue->Typ->Base != TypeStruct && DerefValue->Typ->Base != TypeUnion)
                        ProgramFail(Parser, "can't use '->' on something that's not a struct or union");
                        
                    if (!TableGet(DerefValue->Typ->Members, Ident->Val->Identifier, &CurrentValue))
                        ProgramFail(Parser, "structure doesn't have a member called '%s'", Ident->Val->Identifier);
                    
                    VariableStackPop(Parser, TotalValue);
                    TotalValue = VariableAllocValueFromExistingData(Parser, CurrentValue->Typ, TotalValueData + CurrentValue->Val->Integer, TRUE, DerefValue);
                }
                continue;
            }
            case TokenAssign: 
                LexGetToken(Parser, NULL, TRUE);
            
                if (!ExpressionParse(Parser, &CurrentValue))
                    ProgramFail(Parser, "expression expected");
                    
                if (Parser->Mode == RunModeRun)
                { /* do the assignment */
                    if (!TotalValue->IsLValue)
                        ProgramFail(Parser, "can't assign to this");

                    if (CurrentValue->Typ != TotalValue->Typ)
                        ProgramFail(Parser, "can't assign incompatible types");

                    if (TotalValue->Typ->Base != TypeArray)
                        memcpy((void *)TotalValue->Val, (void *)CurrentValue->Val, TotalValue->Typ->Sizeof);
                    else
                    { /* array assignment */
                        if (TotalValue->Val->Array.Size != CurrentValue->Val->Array.Size)
                            ProgramFail(Parser, "incompatible array sizes in assignment");
                        
                        // XXX - fix this memcpy(TotalValue->Val->Array.Data, CurrentValue->Val->Array.Data, CurrentValue->Typ->Sizeof * CurrentValue->Val->Array.Size);
                    }
                    VariableStackPop(Parser, CurrentValue);
                    *Result = TotalValue;
                }
                return TRUE;
                
            case TokenAddAssign: case TokenSubtractAssign:
                LexGetToken(Parser, NULL, TRUE);
            
                if (!ExpressionParse(Parser, &CurrentValue))
                    ProgramFail(Parser, "expression expected");
                
                if (Parser->Mode == RunModeRun)
                { /* do the assignment */
                    if (!TotalValue->IsLValue)
                        ProgramFail(Parser, "can't assign");

                    if (CurrentValue->Typ->Base == TypeInt && TotalValue->Typ->Base == TypeInt)
                    {
                        switch (Token)
                        {
                            case TokenAddAssign: TotalValue->Val->Integer += CurrentValue->Val->Integer; break;
                            case TokenSubtractAssign: TotalValue->Val->Integer -= CurrentValue->Val->Integer; break;
                            default: break;
                        }
                        VariableStackPop(Parser, CurrentValue);
                    }
#ifndef NO_FP
                    else if (CurrentValue->Typ->Base == TypeFP && TotalValue->Typ->Base == TypeFP)
                    {
                        switch (Token)
                        {
                            case TokenAddAssign: TotalValue->Val->FP += CurrentValue->Val->FP; break;
                            case TokenSubtractAssign: TotalValue->Val->FP -= CurrentValue->Val->FP; break;
                            default: break;
                        }
                        VariableStackPop(Parser, CurrentValue);
                    }
#endif
                    else
                        ProgramFail(Parser, "can't operate and assign these types");
                }
                // fallthrough
            
            default:
                if (Parser->Mode == RunModeRun)
                    *Result = TotalValue;
                return TRUE;
        }
        
        if (!ExpressionParseValue(Parser, &CurrentValue))
            return FALSE;

        if (Parser->Mode == RunModeRun)
        {
#ifndef NO_FP
            if (CurrentValue->Typ->Base == TypeFP || TotalValue->Typ->Base == TypeFP)
            { /* floating point expression */
                double FPTotal, FPCurrent, FPResult;

                if (CurrentValue->Typ->Base != TypeFP || TotalValue->Typ->Base != TypeFP)
                { /* convert both to floating point */
                    if (CurrentValue->Typ->Base == TypeInt)
                        FPCurrent = (double)CurrentValue->Val->Integer;
                    else if (CurrentValue->Typ->Base == TypeFP)
                        FPCurrent = CurrentValue->Val->FP;
                    else
                        ProgramFail(Parser, "bad type for operator");
                        
                    if (TotalValue->Typ->Base == TypeInt)
                        FPTotal = (double)TotalValue->Val->Integer;
                    else if (TotalValue->Typ->Base == TypeFP)
                        FPTotal = TotalValue->Val->FP;
                    else
                        ProgramFail(Parser, "bad type for operator");
                }

                VariableStackPop(Parser, CurrentValue);
                VariableStackPop(Parser, TotalValue);
                
                switch (Token)
                {
                    case TokenPlus:         FPResult = FPTotal + FPCurrent; break;
                    case TokenMinus:        FPResult = FPTotal - FPCurrent; break;
                    case TokenAsterisk:     FPResult = FPTotal * FPCurrent; break;
                    case TokenSlash:        FPResult = FPTotal / FPCurrent; break;
                    case TokenEqual:        FPResult = FPTotal == FPCurrent; break;
                    case TokenLessThan:     FPResult = FPTotal < FPCurrent; break;
                    case TokenGreaterThan:  FPResult = FPTotal > FPCurrent; break;
                    case TokenLessEqual:    FPResult = FPTotal <= FPCurrent; break;
                    case TokenGreaterEqual: FPResult = FPTotal >= FPCurrent; break;
                    case TokenLogicalAnd: case TokenLogicalOr: case TokenAmpersand: case TokenArithmeticOr: case TokenArithmeticExor: ProgramFail(Parser, "bad type for operator"); break;
                    default: break;
                }
                TotalValue = ParsePushFP(Parser, FPResult);
            }
            else
#endif
            { /* integer expression */
                int IntX, IntY, IntResult;
                
                if (CurrentValue->Typ->Base != TypeInt || TotalValue->Typ->Base != TypeInt)
                    ProgramFail(Parser, "bad operand types");
            
                IntX = TotalValue->Val->Integer;
                IntY = CurrentValue->Val->Integer;
                VariableStackPop(Parser, CurrentValue);
                VariableStackPop(Parser, TotalValue);
                
                /* integer arithmetic */
                switch (Token)
                {
                    case TokenPlus:             IntResult = IntX + IntY; break;
                    case TokenMinus:            IntResult = IntX - IntY; break;
                    case TokenAsterisk:         IntResult = IntX * IntY; break;
                    case TokenSlash:            IntResult = IntX / IntY; break;
                    case TokenEqual:            IntResult = IntX == IntY; break;
                    case TokenLessThan:         IntResult = IntX < IntY; break;
                    case TokenGreaterThan:      IntResult = IntX > IntY; break;
                    case TokenLessEqual:        IntResult = IntX <= IntY; break;
                    case TokenGreaterEqual:     IntResult = IntX >= IntY; break;
                    case TokenLogicalAnd:       IntResult = IntX && IntY; break;
                    case TokenLogicalOr:        IntResult = IntX || IntY; break;
                    case TokenAmpersand:        IntResult = IntX & IntY; break;
                    case TokenArithmeticOr:     IntResult = IntX | IntY; break;
                    case TokenArithmeticExor:   IntResult = IntX ^ IntY; break;
                    default:                    IntResult = 0; break;
                }
                TotalValue = ParsePushInt(Parser, IntResult);
            }
            
            *Result = TotalValue;
        }
    }
    
    return TRUE;
}

/* do a function call */
void ExpressionParseFunctionCall(struct ParseState *Parser, struct Value **Result, const char *FuncName)
{
    struct Value *FuncValue;
    struct Value *Param;
    struct Value **ParamArray = NULL;
    int ArgCount;
    enum LexToken Token = LexGetToken(Parser, NULL, TRUE);    /* open bracket */
    
    if (Parser->Mode == RunModeRun) 
    { /* get the function definition */
        VariableGet(Parser, FuncName, &FuncValue);
        if (FuncValue->Typ->Base != TypeFunction)
            ProgramFail(Parser, "not a function - can't call");
    
        *Result = VariableAllocValueFromType(Parser, FuncValue->Val->FuncDef.ReturnType, FALSE, NULL);
        HeapPushStackFrame();
        ParamArray = HeapAllocStack(sizeof(struct Value *) * FuncValue->Val->FuncDef.NumParams);
        if (ParamArray == NULL)
            ProgramFail(Parser, "out of memory");
    }
        
    /* parse arguments */
    ArgCount = 0;
    do {
        if (ExpressionParse(Parser, &Param))
        {
            if (Parser->Mode == RunModeRun)
            { 
                if (ArgCount >= FuncValue->Val->FuncDef.NumParams)
                {
                    if (!FuncValue->Val->FuncDef.VarArgs)
                        ProgramFail(Parser, "too many arguments to %s()", FuncName);
                }
                else
                {
                    if (FuncValue->Val->FuncDef.ParamType[ArgCount] != Param->Typ)
                        ProgramFail(Parser, "parameter %d to %s() is the wrong type", ArgCount+1, FuncName);
                }
                
                if (ArgCount < FuncValue->Val->FuncDef.NumParams)
                    ParamArray[ArgCount] = Param;
            }
            
            ArgCount++;
            Token = LexGetToken(Parser, NULL, TRUE);
            if (Token != TokenComma && Token != TokenCloseBracket)
                ProgramFail(Parser, "comma expected");
        }
        else
        { /* end of argument list? */
            Token = LexGetToken(Parser, NULL, TRUE);
            if (!TokenCloseBracket)
                ProgramFail(Parser, "bad argument");
        }
    } while (Token != TokenCloseBracket);
    
    if (Parser->Mode == RunModeRun) 
    { /* run the function */
        if (ArgCount < FuncValue->Val->FuncDef.NumParams)
            ProgramFail(Parser, "not enough arguments to '%s'", FuncName);
        
        if (FuncValue->Val->FuncDef.Intrinsic == NULL)
        { /* run a user-defined function */
            struct ParseState FuncParser = FuncValue->Val->FuncDef.Body;
            int Count;
            
            VariableStackFrameAdd(Parser, FuncValue->Val->FuncDef.Intrinsic ? FuncValue->Val->FuncDef.NumParams : 0);
            TopStackFrame->NumParams = ArgCount;
            TopStackFrame->ReturnValue = *Result;
            for (Count = 0; Count < FuncValue->Val->FuncDef.NumParams; Count++)
                VariableDefine(Parser, FuncValue->Val->FuncDef.ParamName[Count], ParamArray[Count]);
                
            if (!ParseStatement(&FuncParser))
                ProgramFail(&FuncParser, "function body expected");
        
            if (FuncValue->Val->FuncDef.ReturnType != (*Result)->Typ)
                ProgramFail(&FuncParser, "bad type of return value");

            VariableStackFramePop(Parser);
        }
        else
            FuncValue->Val->FuncDef.Intrinsic(Parser, *Result, ParamArray, ArgCount);

        HeapPopStackFrame();
    }
}
#else

/* local prototypes */
enum OperatorOrder
{
    OrderNone,
    OrderPrefix,
    OrderInfix,
    OrderPostfix
};

/* a stack of expressions we use in evaluation */
struct ExpressionStack
{
    struct ExpressionStack *Next;       /* the next lower item on the stack */
    union
    {
        struct Value *Val;              /* the value for this stack node */
        enum LexToken Op;               /* the operator */
    } p;
    short unsigned int Precedence;      /* the operator precedence of this node */
    unsigned char Order;                /* the evaluation order of this operator */
};

/* operator precedence definitions */
struct OpPrecedence
{
    unsigned char PrefixPrecedence:4;
    unsigned char PostfixPrecedence:4;
    unsigned char InfixPrecedence:4;
    char *Name;
};

static struct OpPrecedence OperatorPrecedence[] =
{
    /* TokenNone, */ { 0, 0, 0, "none" },
    /* TokenComma, */ { 0, 0, 0, "," },
    /* TokenAssign, */ { 0, 0, 2, "=" }, /* TokenAddAssign, */ { 0, 0, 2, "+=" }, /* TokenSubtractAssign, */ { 0, 0, 2, "-=" }, 
    /* TokenMultiplyAssign, */ { 0, 0, 2, "*=" }, /* TokenDivideAssign, */ { 0, 0, 2, "/=" }, /* TokenModulusAssign, */ { 0, 0, 2, "%=" },
    /* TokenShiftLeftAssign, */ { 0, 0, 2, "<<=" }, /* TokenShiftRightAssign, */ { 0, 0, 2, ">>=" }, /* TokenArithmeticAndAssign, */ { 0, 0, 2, "&=" }, 
    /* TokenArithmeticOrAssign, */ { 0, 0, 2, "|=" }, /* TokenArithmeticExorAssign, */ { 0, 0, 2, "^=" },
    /* TokenQuestionMark, */ { 0, 0, 3, "?" }, /* TokenColon, */ { 0, 0, 3, ":" },
    /* TokenLogicalOr, */ { 0, 0, 4, "||" },
    /* TokenLogicalAnd, */ { 0, 0, 5, "&&" },
    /* TokenArithmeticOr, */ { 0, 0, 6, "|" },
    /* TokenArithmeticExor, */ { 0, 0, 7, "^" },
    /* TokenAmpersand, */ { 14, 0, 8, "&" },
    /* TokenEqual, */  { 0, 0, 9, "==" }, /* TokenNotEqual, */ { 0, 0, 9, "!=" },
    /* TokenLessThan, */ { 0, 0, 10, "<" }, /* TokenGreaterThan, */ { 0, 0, 10, ">" }, /* TokenLessEqual, */ { 0, 0, 10, "<=" }, /* TokenGreaterEqual, */ { 0, 0, 10, ">=" },
    /* TokenShiftLeft, */ { 0, 0, 11, "<<" }, /* TokenShiftRight, */ { 0, 0, 11, ">>" },
    /* TokenPlus, */ { 14, 0, 12, "+" }, /* TokenMinus, */ { 14, 0, 12, "-" },
    /* TokenAsterisk, */ { 14, 0, 13, "*" }, /* TokenSlash, */ { 0, 0, 13, "/" }, /* TokenModulus, */ { 0, 0, 13, "%" },
    /* TokenIncrement, */ { 14, 15, 0, "++" }, /* TokenDecrement, */ { 14, 15, 0, "--" }, /* TokenUnaryNot, */ { 14, 0, 0, "!" }, /* TokenUnaryExor, */ { 14, 0, 0, "~" }, /* TokenSizeof, */ { 14, 0, 0, "sizeof" },
    /* TokenLeftSquareBracket, */ { 0, 0, 15, "[" }, /* TokenRightSquareBracket, */ { 0, 15, 0, "]" }, /* TokenDot, */ { 0, 0, 15, "." }, /* TokenArrow, */ { 0, 0, 15, "->" },
    /* TokenOpenBracket, */ { 15, 0, 0, "(" }, /* TokenCloseBracket, */ { 0, 15, 0, ")" }
};

void ExpressionParseFunctionCall(struct ParseState *Parser, struct ExpressionStack **StackTop, const char *FuncName);

#ifdef DEBUG_EXPRESSIONS
/* show the contents of the expression stack */
void ExpressionStackShow(struct ExpressionStack *StackTop)
{
    printf("Expression stack: ");
    
    while (StackTop != NULL)
    {
        if (StackTop->Order == OrderNone)
        { 
            /* it's a value */
            if (StackTop->p.Val->IsLValue)
                printf("lvalue=");
            else
                printf("value=");
                
            switch (StackTop->p.Val->Typ->Base)
            {
                case TypeVoid:      printf("void"); break;
                case TypeInt: case TypeChar: printf("%d:int", StackTop->p.Val->Val->Integer); break;
                case TypeFP:        printf("%f:fp", StackTop->p.Val->Val->FP); break;
                case TypeFunction:  printf("%s:function", StackTop->p.Val->Val->Identifier); break;
                case TypeMacro:     printf("%s:macro", StackTop->p.Val->Val->Identifier); break;
                case TypePointer:
                    if (StackTop->p.Val->Typ->FromType->Base == TypeChar)
                        printf("\"%s\":string", (char *)StackTop->p.Val->Val->Pointer.Segment->Val->Array.Data);
                    else
                        printf("ptr(0x%lx,%d)", (long)StackTop->p.Val->Val->Pointer.Segment, StackTop->p.Val->Val->Pointer.Offset); 
                    break;
                case TypeArray:     printf("array"); break;
                case TypeStruct:    printf("%s:struct", StackTop->p.Val->Val->Identifier); break;
                case TypeUnion:     printf("%s:union", StackTop->p.Val->Val->Identifier); break;
                case TypeEnum:      printf("%s:enum", StackTop->p.Val->Val->Identifier); break;
                default:            printf("unknown"); break;
            }
        }
        else
        { /* it's an operator */
            printf("op='%s' %s %d", OperatorPrecedence[(int)StackTop->p.Op].Name, 
                (StackTop->Order == OrderPrefix) ? "prefix" : ((StackTop->Order == OrderPostfix) ? "postfix" : "infix"), 
                StackTop->Precedence);
        }
        
        StackTop = StackTop->Next;
        if (StackTop != NULL)
            printf(", ");
    }
    
    printf("\n");
}
#endif

/* push a node on to the expression stack */
void ExpressionStackPushValueNode(struct ParseState *Parser, struct ExpressionStack **StackTop, struct Value *ValueLoc)
{
    struct ExpressionStack *StackNode = VariableAlloc(Parser, sizeof(struct ExpressionStack), FALSE);
    StackNode->Next = *StackTop;
    StackNode->p.Val = ValueLoc;
    *StackTop = StackNode;
#ifdef DEBUG_EXPRESSIONS
    ExpressionStackShow(*StackTop);
#endif
}

/* push a blank value on to the expression stack by type */
void ExpressionStackPushValueByType(struct ParseState *Parser, struct ExpressionStack **StackTop, struct ValueType *PushType)
{
    struct Value *ValueLoc = VariableAllocValueFromType(Parser, PushType, FALSE, NULL);
    debugf("ExpressionStackPushValueByType()\n");
    ExpressionStackPushValueNode(Parser, StackTop, ValueLoc);
}

/* push a value on to the expression stack */
void ExpressionStackPushValue(struct ParseState *Parser, struct ExpressionStack **StackTop, struct Value *PushValue)
{
    struct Value *ValueLoc = VariableAllocValueAndCopy(Parser, PushValue, FALSE);
    debugf("ExpressionStackPushValue()\n");
    ExpressionStackPushValueNode(Parser, StackTop, ValueLoc);
}

void ExpressionStackPushLValue(struct ParseState *Parser, struct ExpressionStack **StackTop, struct Value *PushValue)
{
    struct Value *ValueLoc = VariableAllocValueShared(Parser, PushValue);
    debugf("ExpressionStackPushLValue()\n");
    ExpressionStackPushValueNode(Parser, StackTop, ValueLoc);
}

void ExpressionPushInt(struct ParseState *Parser, struct ExpressionStack **StackTop, int IntValue)
{
    struct Value *ValueLoc = VariableAllocValueFromType(Parser, &IntType, FALSE, NULL);
    ValueLoc->Val->Integer = IntValue;
    debugf("ExpressionPushInt()\n");
    ExpressionStackPushValueNode(Parser, StackTop, ValueLoc);
}

void ExpressionPushFP(struct ParseState *Parser, struct ExpressionStack **StackTop, double FPValue)
{
    struct Value *ValueLoc = VariableAllocValueFromType(Parser, &FPType, FALSE, NULL);
    ValueLoc->Val->FP = FPValue;
    debugf("ExpressionPushFP()\n");
    ExpressionStackPushValueNode(Parser, StackTop, ValueLoc);
}

/* evaluate a prefix operator */
void ExpressionPrefixOperator(struct ParseState *Parser, struct ExpressionStack **StackTop, enum LexToken Op, struct Value *TopValue)
{
    struct Value *TempLValue;
    struct Value *Result;

    if (Parser->Mode != RunModeRun)
    {
        /* we're not running it so just return 0 */
        ExpressionPushInt(Parser, StackTop, 0);
        return;
    }
    
    debugf("ExpressionPrefixOperator()\n");
    switch (Op)
    {
        case TokenAmpersand:
            if (!TopValue->IsLValue)
                ProgramFail(Parser, "can't get the address of this");

            TempLValue = TopValue->LValueFrom;
            Result = VariableAllocValueFromType(Parser, TypeGetMatching(Parser, TopValue->Typ, TypePointer, 0, StrEmpty), FALSE, NULL);
            Result->Val->Pointer.Segment = TempLValue;
            Result->Val->Pointer.Offset = (void *)Result->Val - (void *)Result->LValueFrom;
            ExpressionStackPushValueNode(Parser, StackTop, Result);
            break;

        case TokenAsterisk:
            if (TopValue->Typ->Base != TypePointer)
                ProgramFail(Parser, "can't dereference this non-pointer");
            
            ExpressionStackPushLValue(Parser, StackTop, TopValue->Val->Pointer.Segment);
            break;
        
        case TokenSizeof:
            // XXX
            break;
        
        case TokenLeftSquareBracket:
            // XXX
            break;
        
        case TokenOpenBracket:
            // XXX - cast
            break;

        default:
            /* an arithmetic operator */
            if (IS_INTEGER_COERCIBLE(TopValue))
            {
                /* integer prefix arithmetic */
                int ResultInt;
                int TopInt = COERCE_INTEGER(TopValue);
                switch (Op)
                {
                    case TokenPlus:         ResultInt = TopInt; break;
                    case TokenMinus:        ResultInt = -TopInt; break;
                    case TokenIncrement:    ASSIGN_INT(TopValue, TopInt+1); break;
                    case TokenDecrement:    ASSIGN_INT(TopValue, TopInt-1); break;
                    case TokenUnaryNot:     ResultInt = !TopInt; break;
                    case TokenUnaryExor:    ResultInt = ~TopInt; break;
                    default:                ProgramFail(Parser, "invalid operation"); break;
                }

                ExpressionPushInt(Parser, StackTop, ResultInt);
            }
#ifndef NO_FP
            else if (TopValue->Typ == &FPType)
            {
                /* floating point prefix arithmetic */
                double ResultFP;
                switch (Op)
                {
                    case TokenPlus:         ResultFP = TopValue->Val->FP; break;
                    case TokenMinus:        ResultFP = -TopValue->Val->FP; break;
                    default:                ProgramFail(Parser, "invalid operation"); break;
                }
            }
#endif
#if 0
XXX - finish this
            else
            {
                /* pointer prefix arithmetic */
                int TopInt = COERCE_INTEGER(TopValue);
            }
#endif
            break;
    }
}

/* evaluate a postfix operator */
void ExpressionPostfixOperator(struct ParseState *Parser, struct ExpressionStack **StackTop, enum LexToken Op, struct Value *TopValue)
{
    if (Parser->Mode != RunModeRun)
    {
        /* we're not running it so just return 0 */
        ExpressionPushInt(Parser, StackTop, 0);
        return;
    }
    
    debugf("ExpressionPostfixOperator()\n");
    if (IS_INTEGER_COERCIBLE(TopValue))
    {
        int ResultInt;
        int TopInt = COERCE_INTEGER(TopValue);
        switch (Op)
        {
            case TokenIncrement:            ASSIGN_INT_AFTER(TopValue, TopInt+1); break;
            case TokenDecrement:            ASSIGN_INT_AFTER(TopValue, TopInt+1); break;
            case TokenRightSquareBracket:   break;  // XXX
            case TokenCloseBracket:         break;  // XXX
            default:                        ProgramFail(Parser, "invalid operation"); break;
        }
    
        ExpressionPushInt(Parser, StackTop, ResultInt);
    }
}

/* evaluate an infix operator */
void ExpressionInfixOperator(struct ParseState *Parser, struct ExpressionStack **StackTop, enum LexToken Op, struct Value *BottomValue, struct Value *TopValue)
{
    int ResultIsInt = FALSE;
    int ResultInt;

    if (Parser->Mode != RunModeRun)
    {
        /* we're not running it so just return 0 */
        ExpressionPushInt(Parser, StackTop, 0);
        return;
    }
    
    debugf("ExpressionInfixOperator()\n");
    if (Op == TokenLeftSquareBracket)
    { 
        /* array index */
        int ArrayIndex;
        struct Value *Result;
        
        if (BottomValue->Typ->Base != TypeArray)
            ProgramFail(Parser, "not an array");
        
        if (!IS_INTEGER_COERCIBLE(TopValue))
            ProgramFail(Parser, "array index must be an integer");
        
        ArrayIndex = COERCE_INTEGER(TopValue);
        if (ArrayIndex < 0 || ArrayIndex >= BottomValue->Val->Array.Size)
            ProgramFail(Parser, "illegal array index %d [0..%d]", ArrayIndex, BottomValue->Val->Array.Size-1);
        
        /* make the array element result */
        Result = VariableAllocValueFromExistingData(Parser, BottomValue->Typ->FromType, (union AnyValue *)(BottomValue->Val->Array.Data + TypeSize(BottomValue->Typ->FromType, 0) * ArrayIndex), BottomValue->IsLValue, BottomValue->LValueFrom);
        ExpressionStackPushValueNode(Parser, StackTop, Result);
    }
    else if (IS_INTEGER_COERCIBLE(TopValue) && IS_INTEGER_COERCIBLE(BottomValue))
    { 
        /* integer operation */
        int TopInt = COERCE_INTEGER(TopValue);
        int BottomInt = COERCE_INTEGER(BottomValue);
        switch (Op)
        {
            case TokenAssign:               ASSIGN_INT(BottomValue, TopInt); break;
            case TokenAddAssign:            ASSIGN_INT(BottomValue, BottomInt + TopInt); break;
            case TokenSubtractAssign:       ASSIGN_INT(BottomValue, BottomInt - TopInt); break;
            case TokenMultiplyAssign:       ASSIGN_INT(BottomValue, BottomInt * TopInt); break;
            case TokenDivideAssign:         ASSIGN_INT(BottomValue, BottomInt / TopInt); break;
            case TokenModulusAssign:        ASSIGN_INT(BottomValue, BottomInt % TopInt); break;
            case TokenShiftLeftAssign:      ASSIGN_INT(BottomValue, BottomInt << TopInt); break;
            case TokenShiftRightAssign:     ASSIGN_INT(BottomValue, BottomInt >> TopInt); break;
            case TokenArithmeticAndAssign:  ASSIGN_INT(BottomValue, BottomInt & TopInt); break;
            case TokenArithmeticOrAssign:   ASSIGN_INT(BottomValue, BottomInt | TopInt); break;
            case TokenArithmeticExorAssign: ASSIGN_INT(BottomValue, BottomInt ^ TopInt); break;
            case TokenQuestionMark:         break; // XXX
            case TokenColon:                break; // XXX
            case TokenLogicalOr:            ResultInt = BottomInt || TopInt; break;
            case TokenLogicalAnd:           ResultInt = BottomInt && TopInt; break;
            case TokenArithmeticOr:         ResultInt = BottomInt | TopInt; break;
            case TokenArithmeticExor:       ResultInt = BottomInt ^ TopInt; break;
            case TokenAmpersand:            ResultInt = BottomInt & TopInt; break;
            case TokenEqual:                ResultInt = BottomInt == TopInt; break;
            case TokenNotEqual:             ResultInt = BottomInt != TopInt; break;
            case TokenLessThan:             ResultInt = BottomInt < TopInt; break;
            case TokenGreaterThan:          ResultInt = BottomInt > TopInt; break;
            case TokenLessEqual:            ResultInt = BottomInt <= TopInt; break;
            case TokenGreaterEqual:         ResultInt = BottomInt >= TopInt; break;
            case TokenShiftLeft:            ResultInt = BottomInt << TopInt; break;
            case TokenShiftRight:           ResultInt = BottomInt << TopInt; break;
            case TokenPlus:                 ResultInt = BottomInt + TopInt; break;
            case TokenMinus:                ResultInt = BottomInt - TopInt; break;
            case TokenAsterisk:             ResultInt = BottomInt * TopInt; break;
            case TokenSlash:                ResultInt = BottomInt / TopInt; break;
            case TokenModulus:              ResultInt = BottomInt % TopInt; break;
            default:                        ProgramFail(Parser, "invalid operation"); break;
        }
        
        ExpressionPushInt(Parser, StackTop, ResultInt);
    }
#ifndef NO_FP
    else if ( (TopValue->Typ == &FPType && BottomValue->Typ == &FPType) ||
              (TopValue->Typ == &FPType && IS_INTEGER_COERCIBLE(BottomValue)) ||
              (IS_INTEGER_COERCIBLE(TopValue) && BottomValue->Typ == &FPType) )
    {
        /* floating point infix arithmetic */
        double ResultFP;
        double TopFP = (TopValue->Typ == &FPType) ? TopValue->Val->FP : (double)COERCE_INTEGER(TopValue);
        double BottomFP = (BottomValue->Typ == &FPType) ? BottomValue->Val->FP : (double)COERCE_INTEGER(BottomValue);

        switch (Op)
        {
            case TokenAssign:               ASSIGN_FP(BottomValue, TopFP); break;
            case TokenAddAssign:            ASSIGN_FP(BottomValue, BottomFP + TopFP); break;
            case TokenSubtractAssign:       ASSIGN_FP(BottomValue, BottomFP - TopFP); break;
            case TokenMultiplyAssign:       ASSIGN_FP(BottomValue, BottomFP * TopFP); break;
            case TokenDivideAssign:         ASSIGN_FP(BottomValue, BottomFP / TopFP); break;
            case TokenEqual:                ResultInt = BottomFP == TopFP; ResultIsInt = TRUE; break;
            case TokenNotEqual:             ResultInt = BottomFP != TopFP; ResultIsInt = TRUE; break;
            case TokenLessThan:             ResultInt = BottomFP < TopFP; ResultIsInt = TRUE; break;
            case TokenGreaterThan:          ResultInt = BottomFP > TopFP; ResultIsInt = TRUE; break;
            case TokenLessEqual:            ResultInt = BottomFP <= TopFP; ResultIsInt = TRUE; break;
            case TokenGreaterEqual:         ResultInt = BottomFP >= TopFP; ResultIsInt = TRUE; break;
            case TokenPlus:                 ResultFP = BottomFP + TopFP; break;
            case TokenMinus:                ResultFP = BottomFP - TopFP; break;
            case TokenAsterisk:             ResultFP = BottomFP * TopFP; break;
            case TokenSlash:                ResultFP = BottomFP / TopFP; break;
            default:                        ProgramFail(Parser, "invalid operation"); break;
        }

        if (ResultIsInt)
            ExpressionPushInt(Parser, StackTop, ResultInt);
        else
            ExpressionPushFP(Parser, StackTop, ResultFP);
    }
#endif
#if 0
XXX - finish this
    else if ( (TopValue->Typ->Base == TypePointer && IS_INTEGER_COERCIBLE(BottomValue)) ||
              (IS_INTEGER_COERCIBLE(TopValue) && BottomValue->Typ->Base == TypePointer) )
    {
        /* pointer infix arithmetic */
        
        switch (TopOperatorNode->Op)
        {
            case TokenEqual:                ResultInt = BottomInt == TopInt; ResultIsInt = TRUE; break;
            case TokenNotEqual:             ResultInt = BottomInt != TopInt; ResultIsInt = TRUE; break;
            case TokenLessThan:             ResultInt = BottomInt < TopInt; ResultIsInt = TRUE; break;
            case TokenGreaterThan:          ResultInt = BottomInt > TopInt; ResultIsInt = TRUE; break;
            case TokenLessEqual:            ResultInt = BottomInt <= TopInt; ResultIsInt = TRUE; break;
            case TokenGreaterEqual:         ResultInt = BottomInt >= TopInt; ResultIsInt = TRUE; break;
            case TokenPlus:                 Result = BottomInt + TopInt; break;
            case TokenMinus:                Result = BottomInt - TopInt; break;
            default:                        ProgramFail(Parser, "invalid operation"); break;
        }
        
        if (ResultIsInt)
            ExpressionPushInt(Parser, StackTop, ResultInt);
        else
            ExpressionPushPointer(Parser, StackTop, ResultInt);
    }
#endif
    else
        ProgramFail(Parser, "invalid operation");
}

/* take the contents of the expression stack and compute the top until there's nothing greater than the given precedence */
void ExpressionStackCollapse(struct ParseState *Parser, struct ExpressionStack **StackTop, int Precedence)
{
    int FoundPrecedence = Precedence;
    struct Value *TopValue;
    struct Value *BottomValue;
    struct ExpressionStack *TopStackNode = *StackTop;
    struct ExpressionStack *TopOperatorNode;
    
    debugf("ExpressionStackCollapse():\n");
#ifdef DEBUG_EXPRESSIONS
    ExpressionStackShow(*StackTop);
#endif
    while (TopStackNode != NULL && TopStackNode->Next != NULL && FoundPrecedence >= Precedence)
    {
        /* find the top operator on the stack */
        if (TopStackNode->Order == OrderNone)
            TopOperatorNode = TopStackNode->Next;
        else
            TopOperatorNode = TopStackNode;
        
        FoundPrecedence = TopOperatorNode->Precedence;
        
        /* does it have a high enough precedence? */
        if (FoundPrecedence >= Precedence && TopOperatorNode != NULL)
        {
            /* execute this operator */
            switch (TopOperatorNode->Order)
            {
                case OrderPrefix:
                    /* prefix evaluation */
                    debugf("prefix evaluation\n");
                    TopValue = TopStackNode->p.Val;
                    
                    /* pop the value and then the prefix operator - assume they'll still be there until we're done */
                    HeapPopStack(TopOperatorNode, sizeof(struct ExpressionStack)*2 + sizeof(struct Value) + TypeStackSizeValue(TopValue));
                    *StackTop = TopOperatorNode->Next;
                    
                    /* do the prefix operation */
                    ExpressionPrefixOperator(Parser, StackTop, TopOperatorNode->p.Op, TopValue);
                    break;
                
                case OrderPostfix:
                    /* postfix evaluation */
                    debugf("postfix evaluation\n");
                    TopValue = TopStackNode->Next->p.Val;
                    
                    /* pop the postfix operator and then the value - assume they'll still be there until we're done */
                    HeapPopStack(TopValue, sizeof(struct ExpressionStack)*2 + sizeof(struct Value) + TypeStackSizeValue(TopValue));
                    *StackTop = TopStackNode->Next->Next;

                    /* do the postfix operation */
                    ExpressionPostfixOperator(Parser, StackTop, TopOperatorNode->p.Op, TopValue);
                    break;
                
                case OrderInfix:
                    /* infix evaluation */
                    debugf("infix evaluation\n");
                    TopValue = TopStackNode->p.Val;
                    BottomValue = TopOperatorNode->Next->p.Val;
                    
                    /* pop a value, the operator and another value - assume they'll still be there until we're done */
                    HeapPopStack(BottomValue, sizeof(struct ExpressionStack)*3 + sizeof(struct Value)*2 + TypeStackSizeValue(TopValue) + TypeStackSizeValue(BottomValue));
                    *StackTop = TopOperatorNode->Next->Next;
                    
                    /* do the infix operation */
                    ExpressionInfixOperator(Parser, StackTop, TopOperatorNode->p.Op, BottomValue, TopValue);
                    break;

                case OrderNone:
                    break;
            }
        }
#ifdef DEBUG_EXPRESSIONS
        ExpressionStackShow(*StackTop);
#endif
        TopStackNode = *StackTop;
    }
    debugf("ExpressionStackCollapse() finished\n");
}

/* push an operator on to the expression stack */
void ExpressionStackPushOperator(struct ParseState *Parser, struct ExpressionStack **StackTop, enum OperatorOrder Order, enum LexToken Token, int Precedence)
{
    struct ExpressionStack *StackNode = VariableAlloc(Parser, sizeof(struct ExpressionStack), FALSE);
    StackNode->Next = *StackTop;
    StackNode->Order = Order;
    StackNode->p.Op = Token;
    StackNode->Precedence = Precedence;
    *StackTop = StackNode;
    debugf("ExpressionStackPushOperator()\n");
#ifdef DEBUG_EXPRESSIONS
    ExpressionStackShow(*StackTop);
#endif
}

/* do the '.' and '->' operators */
void ExpressionGetStructElement(struct ParseState *Parser, struct ExpressionStack **StackTop, enum LexToken Token)
{
    struct Value *Ident;
    
    /* get the identifier following the '.' or '->' */
    if (LexGetToken(Parser, &Ident, TRUE) != TokenIdentifier)
        ProgramFail(Parser, "need an structure or union member after '%s'", (Token == TokenDot) ? "." : "->");

    if (Parser->Mode == RunModeRun)
    { 
        /* look up the struct element */
        struct Value *StructVal = (*StackTop)->p.Val;
        struct Value *MemberValue;
        struct Value *Result;

        if (Token == TokenArrow)
            ProgramFail(Parser, "'->' not implemented yet");
        
        if (StructVal->Typ->Base != TypeStruct && StructVal->Typ->Base != TypeUnion)
            ProgramFail(Parser, "can't use '%s' on something that's not a struct or union %s", (Token == TokenDot) ? "." : "->", (Token == TokenArrow) ? "pointer" : "");
            
        if (!TableGet(StructVal->Typ->Members, Ident->Val->Identifier, &MemberValue))
            ProgramFail(Parser, "doesn't have a member called '%s'", Ident->Val->Identifier);
        
        /* pop the value - assume it'll still be there until we're done */
        HeapPopStack(StructVal, sizeof(struct ExpressionStack) + sizeof(struct Value) + TypeStackSizeValue(StructVal));
        *StackTop = (*StackTop)->Next;
        
        /* make the result value for this member only */
        Result = VariableAllocValueFromExistingData(Parser, MemberValue->Typ, (void *)StructVal->Val + MemberValue->Val->Integer, TRUE, StructVal->LValueFrom);
        ExpressionStackPushValueNode(Parser, StackTop, Result);
    }
}

/* parse an expression with operator precedence */
int ExpressionParse(struct ParseState *Parser, struct Value **Result)
{
    struct Value *LexValue;
    int PrefixState = TRUE;
    int Done = FALSE;
    int BracketPrecedence = 0;
    int LocalPrecedence;
    int Precedence = 0;
    struct ExpressionStack *StackTop = NULL;
    debugf("ExpressionParse():\n");
    
    do
    {
        struct ParseState PreState = *Parser;
        enum LexToken Token = LexGetToken(Parser, &LexValue, TRUE);
        if ((int)Token > TokenComma && (int)Token <= (int)TokenCloseBracket)
        { 
            /* it's an operator with precedence */
            if (PrefixState)
            { 
                /* expect a prefix operator */
                if (OperatorPrecedence[(int)Token].PrefixPrecedence == 0)
                    ProgramFail(Parser, "operator not expected here");
                
                LocalPrecedence = OperatorPrecedence[(int)Token].PrefixPrecedence;
                Precedence = BracketPrecedence + LocalPrecedence;
                if (Token == TokenOpenBracket)
                { 
                    /* boost the bracket operator precedence, then push */
                    BracketPrecedence += BRACKET_PRECEDENCE;
                    // XXX ExpressionStackPushOperator(Parser, &StackTop, OrderPrefix, Token, Precedence);
                }
                else
                { 
                    /* scan and collapse the stack to the precedence of this operator, then push */
                    ExpressionStackCollapse(Parser, &StackTop, Precedence);
                    ExpressionStackPushOperator(Parser, &StackTop, OrderPrefix, Token, Precedence);
                }
            }
            else
            { 
                /* expect an infix or postfix operator */
                if (OperatorPrecedence[(int)Token].PostfixPrecedence != 0)
                {
                    switch (Token)
                    {
                        case TokenCloseBracket:
                        case TokenRightSquareBracket:
                            if (BracketPrecedence == 0)
                            { 
                                /* assume this bracket is after the end of the expression */
                                *Parser = PreState;
                                Done = TRUE;
                            }
                            else
                                BracketPrecedence -= BRACKET_PRECEDENCE;
                            break;
                    
                        default:
                            /* scan and collapse the stack to the precedence of this operator, then push */
                            Precedence = BracketPrecedence + OperatorPrecedence[(int)Token].PostfixPrecedence;
                            ExpressionStackCollapse(Parser, &StackTop, Precedence);
                            ExpressionStackPushOperator(Parser, &StackTop, OrderPostfix, Token, Precedence);
                            break;
                    }
                }
                else if (OperatorPrecedence[(int)Token].InfixPrecedence != 0)
                { 
                    /* scan and collapse the stack, then push */
                    if (Token == TokenDot || Token == TokenArrow)
                        ExpressionGetStructElement(Parser, &StackTop, Token); /* this operator is followed by a struct element so handle it as a special case */
                    else
                    { 
                        /* a standard infix operator */
                        Precedence = BracketPrecedence + OperatorPrecedence[(int)Token].InfixPrecedence;
                        
                        /* for right to left order, only go down to the next higher precedence so we evaluate it in reverse order */
                        /* for left to right order, collapse down to this precedence so we evaluate it in forward order */
                        if (IS_LEFT_TO_RIGHT(OperatorPrecedence[(int)Token].InfixPrecedence))
                            ExpressionStackCollapse(Parser, &StackTop, Precedence);
                        else
                            ExpressionStackCollapse(Parser, &StackTop, Precedence+1);

                        ExpressionStackPushOperator(Parser, &StackTop, OrderInfix, Token, Precedence);
                        PrefixState = TRUE;
                    }

                    /* treat an open square bracket as an infix array index operator followed by an open bracket */
                    if (Token == TokenLeftSquareBracket)
                    { 
                        /* boost the bracket operator precedence, then push */
                        BracketPrecedence += BRACKET_PRECEDENCE;
                        // XXX ExpressionStackPushOperator(Parser, &StackTop, OrderPrefix, Token, Precedence);
                    }
                }
                else
                    ProgramFail(Parser, "operator not expected here");
            }
        }
        else if (Token == TokenIdentifier)
        { 
            /* it's a variable, function or a macro */
            if (LexGetToken(Parser, NULL, FALSE) == TokenOpenBracket)
                ExpressionParseFunctionCall(Parser, &StackTop, LexValue->Val->Identifier);
            else
            {
                if (Parser->Mode == RunModeRun)
                {
                    struct Value *VariableValue = NULL;
                    
                    VariableGet(Parser, LexValue->Val->Identifier, &VariableValue);
                    if (VariableValue->Typ->Base == TypeMacro)
                    {
                        ProgramFail(Parser, "XXX macros unimplemented");
#if 0
                        struct ParseState MacroParser = VariableValue->Val->Parser;
                        
                        if (!ExpressionParse(&MacroParser, Result) || LexGetToken(&MacroParser, NULL, FALSE) != TokenEndOfFunction)
                            ProgramFail(&MacroParser, "expression expected");
#endif
                    }
                    else if (VariableValue->Typ == TypeVoid)
                        ProgramFail(Parser, "a void value isn't much use here");
                    else
                        ExpressionStackPushLValue(Parser, &StackTop, VariableValue); /* it's a value variable */
                }
                else /* push a dummy value */
                    ExpressionPushInt(Parser, &StackTop, 0);

                PrefixState = FALSE;
            }
        }
        else if ((int)Token > TokenComma && (int)Token <= (int)TokenCharacterConstant)
        { 
            /* it's a value of some sort, push it */
            if (!PrefixState)
                ProgramFail(Parser, "value not expected here");
                
            PrefixState = FALSE;
            ExpressionStackPushValue(Parser, &StackTop, LexValue);
        }
        else
        { 
            /* it isn't a token from an expression */
            *Parser = PreState;
            Done = TRUE;
        }
        
    } while (!Done);
    
    /* scan and collapse the stack to precedence 0 */
    ExpressionStackCollapse(Parser, &StackTop, 0);
    
    /* fix up the stack and return the result if we're in run mode */
    if (StackTop != NULL)
    {
        /* all that should be left is a single value on the stack */
        if (Parser->Mode == RunModeRun)
            *Result = StackTop->p.Val;

        HeapPopStack(StackTop, sizeof(struct ExpressionStack));
    }
    
    debugf("ExpressionParse() done\n");
    return StackTop != NULL;
}


/* do a function call */
void ExpressionParseFunctionCall(struct ParseState *Parser, struct ExpressionStack **StackTop, const char *FuncName)
{
    struct Value *ReturnValue;
    struct Value *FuncValue;
    struct Value *Param;
    struct Value **ParamArray = NULL;
    int ArgCount;
    enum LexToken Token = LexGetToken(Parser, NULL, TRUE);    /* open bracket */
    
    if (Parser->Mode == RunModeRun) 
    { 
        /* get the function definition */
        VariableGet(Parser, FuncName, &FuncValue);
        if (FuncValue->Typ->Base != TypeFunction)
            ProgramFail(Parser, "not a function - can't call");
    
        ExpressionStackPushValueByType(Parser, StackTop, FuncValue->Val->FuncDef.ReturnType);
        ReturnValue = (*StackTop)->p.Val;
        HeapPushStackFrame();
        ParamArray = HeapAllocStack(sizeof(struct Value *) * FuncValue->Val->FuncDef.NumParams);
        if (ParamArray == NULL)
            ProgramFail(Parser, "out of memory");
    }
        
    /* parse arguments */
    ArgCount = 0;
    do {
        if (ExpressionParse(Parser, &Param))
        {
            if (Parser->Mode == RunModeRun)
            { 
                if (ArgCount >= FuncValue->Val->FuncDef.NumParams)
                {
                    if (!FuncValue->Val->FuncDef.VarArgs)
                        ProgramFail(Parser, "too many arguments to %s()", FuncName);
                }
                else
                {
                    if (FuncValue->Val->FuncDef.ParamType[ArgCount] != Param->Typ)
                        ProgramFail(Parser, "parameter %d to %s() is the wrong type", ArgCount+1, FuncName);
                }
                
                if (ArgCount < FuncValue->Val->FuncDef.NumParams)
                    ParamArray[ArgCount] = Param;
            }
            
            ArgCount++;
            Token = LexGetToken(Parser, NULL, TRUE);
            if (Token != TokenComma && Token != TokenCloseBracket)
                ProgramFail(Parser, "comma expected");
        }
        else
        { 
            /* end of argument list? */
            Token = LexGetToken(Parser, NULL, TRUE);
            if (!TokenCloseBracket)
                ProgramFail(Parser, "bad argument");
        }
    } while (Token != TokenCloseBracket);
    
    if (Parser->Mode == RunModeRun) 
    { 
        /* run the function */
        if (ArgCount < FuncValue->Val->FuncDef.NumParams)
            ProgramFail(Parser, "not enough arguments to '%s'", FuncName);
        
        if (FuncValue->Val->FuncDef.Intrinsic == NULL)
        { 
            /* run a user-defined function */
            struct ParseState FuncParser = FuncValue->Val->FuncDef.Body;
            int Count;
            
            VariableStackFrameAdd(Parser, FuncValue->Val->FuncDef.Intrinsic ? FuncValue->Val->FuncDef.NumParams : 0);
            TopStackFrame->NumParams = ArgCount;
            TopStackFrame->ReturnValue = ReturnValue;
            for (Count = 0; Count < FuncValue->Val->FuncDef.NumParams; Count++)
                VariableDefine(Parser, FuncValue->Val->FuncDef.ParamName[Count], ParamArray[Count]);
                
            if (!ParseStatement(&FuncParser))
                ProgramFail(&FuncParser, "function body expected");
        
            if (FuncValue->Val->FuncDef.ReturnType != ReturnValue->Typ)
                ProgramFail(&FuncParser, "bad type of return value");

            VariableStackFramePop(Parser);
        }
        else
            FuncValue->Val->FuncDef.Intrinsic(Parser, ReturnValue, ParamArray, ArgCount);

        HeapPopStackFrame();
    }
}
#endif

/* parse an expression. operator precedence is not supported */
int ExpressionParseInt(struct ParseState *Parser)
{
    struct Value *Val;
    int Result = 0;
    
    if (!ExpressionParse(Parser, &Val))
        ProgramFail(Parser, "expression expected");
    
    if (Parser->Mode == RunModeRun)
    { 
        if (Val->Typ->Base != TypeInt)
            ProgramFail(Parser, "integer value expected");
    
        Result = Val->Val->Integer;
        VariableStackPop(Parser, Val);
    }
    
    return Result;
}

