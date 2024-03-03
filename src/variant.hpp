#pragma once

// Michal Vlasák, FIT CTU, 2024

// This file provides macros and template functions for working with types that
// are one of several "variants". It's kind of like std::variant, but with
// different memory layout, and worse :). The main reason for us to roll our own
// thing is to keep control of the memory layout, have explicit `enum class`
// discriminators ("tags", "kinds"), and e.g. more control on how we downcast.
// Also we would still like to be able to combine inheritance for places where
// it is convenient (like AST, expressions and statements), while having
// variants elsewhere (different variants of expressions or statements). This
// approach has been very much inspired by LLVM [1, 2, 3, 4].
//
// Ultimately, why we want to implement some types with limited set of variants
// is because of the "Expression problem" [5, 6]. Simply put, we are not able to
// make a type with multiple variants and multiple methods extensible to allow
// both adding new variants and new methods. We can either choose the "object
// oriented" approach, and use virtual methods, which then makes it easy to add
// new variants, but hard to add new methods. Or we can choose the "functional"
// approach and have a "sum" type with fixed amount of variants, and make it
// easy to add new functions, but hard to add new variants.
//
// In implementations of programming languages, we usually care about adding new
// _methods_ (print, compile, interpret, deallocate, ...), whereas we mostly
// have a limited set of variants (we usually just add new AST nodes when
// changing the language). So for us, the "functional" approach and form of
// dispatch makes most sense. Like 90 % of Computer Science, most of the
// operations we will be doing will be just "case analysis and structural
// recursion". For that being able to dispatch on the variants is very
// important, and it helps if it is also nice. Put another way, we have a use
// case, where `dynamic_cast` or the visitor pattern is the solution. Though
// it's not nice on the eyes (nor fast), so we want something better (e.g. even
// Java had to do it: [7]).
//
// Because of this we employ a lousy variant of "pattern matching" where we use
// lambda function arguments for the "pattern matching".
//
// [1]: https://stackoverflow.com/questions/5134975/what-can-make-c-rtti-undesirable-to-use/5138926#5138926
// [2]: https://llvm.org/docs/ProgrammersManual.html#isa
// [3]: https://llvm.org/docs/HowToSetUpLLVMStyleRTTI.html
// [4]: https://pvs-studio.com/en/blog/posts/cpp/0998/
//
// [5]: https://wiki.c2.com/?ExpressionProblem
// [6]: https://eli.thegreenplace.net/2016/the-expression-problem-and-its-solutions/
//
// [7]: https://openjdk.org/jeps/441
//
// This file introduces a few macros and template functions to make the usage
// (call sites) "nice". Unfortunately this makes the code in this file not that
// readable, as we employ X macros [8], the `overloaded` trick [9] and other
// weird things.
//
// [8]: https://en.wikipedia.org/wiki/X_macro
// [9]: https://tamir.dev/posts/that-overloaded-trick-overloading-lambdas-in-cpp17/
//
// An example on what the macros generate will hopefully make things
// more clear:
//
// The definition of variants, which "calls back" the VARIANT macro with the
// name of each variant:
/*
//     #define EXPRESSIONS(VARIANT) \
//         VARIANT(AstNil) \
//         VARIANT(AstBoolean) \
//         VARIANT(AstNumber) \
*/
// Later, other macros will get the "list of variants" and the base name of the
// type, and generate something for all the variants. First the discriminator
// enum:
//
//     DEFINE_VARIANT_ENUM(Expression, EXPRESSIONS)
//
// =>
//
//     enum class ExpressionKind {
//         AstNil,
//         AstBoolean,
//         AstNumber,
//     }
//
// Each variant is added with the same name as a member of the enum class. We
// don't strictly need it, but you may have a use case for having the enum and
// all the variants with regular names, if you for example switch on the kind
// yourself. A field with name `kind` should be present on all the variants,
// either through inheritance or explicitly, and the types for variants must be
// named the same way as in the X macro:
//
//     struct Expression {
//         ExpressionKind kind;
//     }
//
//     struct AstNil : public Expression {
//     };
//
//     struct AstBoolean : public Expression {
//         bool value;
//     };
//
//     struct AstNumber : public Expression {
//         double value;
//     };
//
// Then _after_ the types with the same names as the variants are introduced, we
// may use another of our macros:
//
//    DEFINE_VARIANT_KIND_TYPE_MAP(Expression, EXPRESSIONS)
//
// =>
//
//    template <> struct kind_of_type<AstNil> { static constexpr auto KIND = ExpressionKind::AstNil; }
//    template <> struct kind_of_type<AstBoolean> { static constexpr auto KIND = ExpressionKind::AstBoolean; }
//    template <> struct kind_of_type<AstNumber> { static constexpr auto KIND = ExpressionKind::AstNumber; }
//
// This gets us a "type to kind" map which allows us to get from templated code
// get from the type to the kind. This is necessary, because most of our
// provided methods are actually generic templates.
//
// Next we are able to generate a `visit` function:
//
//     DEFINE_VISITOR(Expression, EXPRESSIONS)
//
// The thing we want to accomplish is essentially, to instead of writing this:
//
//     std::string to_str(Expression *expr) {
//         switch (expr->kind) {
//         case ExpressionKind::AstNil:
//             AstNil *nil = (AstNil *) expr;
//             // handle AstNil
//             return ...;
//             break;
//         case ExpressionKind::AstBoolean:
//             AstBoolean *bool = (AstBoolean *) expr;
//             // handle AstBoolean
//             return ...;
//             break;
//         case ExpressionKind::AstNumber:
//             AstNumber *number = (AstNumber *) expr;
//             // handle AstNumber
//             return ...;
//             break;
//         }
//     }
//
// To be able to write this:
//
// std::string print(Expression *expr) {
//     return visit(expr,
//     [&] (AstNil *nil) -> std::string {
//         // handle AstNil
//         return ...;
//     },
//     [&] (AstBoolean *boolean) -> std::string {
//         // handle AstNil
//         return ...;
//     },
//     [&] (AstNumber *number) -> std::string {
//         // handle AstNil
//         return ...;
//     });
// }
//
// The dispatch in the second variant is unlike the first one type safe. But
// both allow easily keeping state (unlike virtual methods) and returning values
// (unlike visitor pattern in C++). One disadvantage of the lambda's though is
// that inference of the return type sometimes doesn't work as nicely (since all
// lambdas have to return one common type), annotating the return type
// explicitly like above is a way to overcome this.
//
// In reality the macro generates this `visit` function:
//
// =>
//
//     template<typename... Fs>
//     decltype(auto) visit(base_type *target, Fs&&... functions)
//     {
//         auto visitor = overloaded(std::forward<Fs>(functions)...);
//         switch (target->kind) {
//         case kind_of_type<AstNil>::KIND: return visitor.operator()((AstNil *) target); break;
//         case kind_of_type<AstBoolean>::KIND: return visitor.operator()((AstBoolean *) target); break;
//         case kind_of_type<AstNumber>::KIND: return visitor.operator()((AstNumber *) target); break;
//         }
//         unreachable();
//     }
//
// This mostly just needs to call the right lambdas (or rather, their
// 'operator()'), for the right cases. For _statically_ dispatching the right
// lambda, we use the "overloaded" trick [8], where we construct an object
// inheriting from all the lambdas, using all their operator() and thus we are
// able to call the same method, just with different arguments and C++ method
// overloading chooses the correct lambda to call.
//
// As a special case, one lambda can accept `auto` argument, which through the
// overloading is able to "catch" the remaining variants. The visitor thus needs
// to be exhaustive (cover all variants), but catch alls through `auto`, like
// the following, are allowed. (Interestingly, in this example `Expression`
// wouldn't work for the catch all, but `auto` does.)
//
//     [&] (auto *unhandled) -> std::string {
//         std::cerr << "Unhandled variant " << unhandled << "\n";
//         unreachable();
//     },
//
// (The return type annotation is needed here for the proper `visit` return type
// deduction, but since `unreachable` is marked as [[noreturn]] we don't need to
// return anything.)
//
// That's all the trickery! The rest is just some utility `isa`, `as` and
// `dyn_cast` functions inspired by LLVM [2]. Our implementations are much more
// limited though, and work with just unqualified pointers. For example usage of
// `dyn_cast` see the parser.
//
// Beware, this has not been written by a C++ expert. All improvements welcome.

#include <cassert>

#define VARIANT_ENUM(variant) variant,
#define DEFINE_VARIANT_ENUM(base_type, variants) \
	enum class base_type##Kind { \
		variants(VARIANT_ENUM) \
	};

template <typename T>
struct kind_of_type;

#define VARIANT_KIND_TYPE(variant) template <> struct kind_of_type<variant> { static constexpr auto KIND = decltype(variant::kind) :: variant; };
#define DEFINE_VARIANT_KIND_TYPE_MAP(base_type, variants) \
	variants(VARIANT_KIND_TYPE)

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

#define VARIANT_CASE(type) case kind_of_type<type>::KIND: return visitor.operator()((type *) target); break;
#define DEFINE_VISITOR(base_type, variants) \
	template<typename... Fs> \
	decltype(auto) visit(base_type *target, Fs&&... functions) \
	{ \
		auto visitor = overloaded(std::forward<Fs>(functions)...); \
		switch (target->kind) { \
		variants(VARIANT_CASE) \
		} \
		unreachable(); \
	}

template <typename To, typename From>
[[nodiscard]] inline bool isa(From *from) {
	return from->kind == kind_of_type<To>::KIND;
}

template <typename To, typename From>
[[nodiscard]] inline To *as(From *from) {
	assert(isa<To>(from) && "checked 'isa' failed");
	return static_cast<To *>(from);
}

template <typename To, typename From>
[[nodiscard]] inline To *dyn_cast(From *from) {
	if (isa<To>(from)) {
		return static_cast<To *>(from);
	}
	return nullptr;
}
