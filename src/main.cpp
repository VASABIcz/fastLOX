#include <cstddef>
#include <variant>

#include "allocator.hpp"
#include "ast.hpp"
#include "parser.hpp"

int main(int argc, const char **argv) {

	if (argc < 2) {
		std::cerr << "Expected file name argument\n";
		return EXIT_FAILURE;
	}

	ArenaAllocator arena(MemoryKind_Static);
	StackingAllocator scratch;

	std::ifstream source_file(argv[1]);
	if (!source_file.is_open()) {
		std::cerr << "Can't read file '" << argv[1] << "'\n";
		return EXIT_FAILURE;
	}
	std::stringstream source;
	source << source_file.rdbuf();
	std::variant<AstProgram, std::vector<ParserError>> parse_result =
		parse(&arena, &scratch, source.str());
	int ret = std::visit(overloaded{
		[&] (AstProgram &ast) {
			// TODO
			return EXIT_SUCCESS;
		},
		[&] (std::vector<ParserError> &errors) {
			for (ParserError &error : errors) {
				error.print_with_context(std::cerr, source.str());
			}
			return EXIT_FAILURE;
		},
	}, parse_result);

	return ret;
}
