/* This parses a JSON object but only interprets top-level strings. Other values
 * are returned as unparsed JSON. */

#ifndef almost_json_parser_hpp
#define almost_json_parser_hpp

#include <map>

namespace almost_json_parser {

constexpr int INPUT_BUFFER_SIZE = 16;

class syntax_error : public std::exception {
public:
    const char *what() const override {
        return "syntax error";
    }
};

struct parsed_value {
    std::string data;
    bool is_string; // if false, "data" is an unparsed JSON value
};

struct parse_state {
    virtual ~parse_state() = 0;
    virtual bool parse(almost_json_parser&,const char*&) = 0;
};

using value_map = std::map<std::string,parsed_value>;

class parser {
    char buffer[INPUT_BUFFER_SIZE];
    int buffered;
    std::vector<std::unique_ptr<parse_state>> stack;

public:
    value_map value;

    parser();

    void feed(const char *input,size_t size);
    void finish();
    void reset();

    void push_state(parse_sate *state);
};

}

#endif

