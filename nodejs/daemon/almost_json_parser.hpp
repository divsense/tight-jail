/* This parses a JSON object or array but only interprets top-level strings.
 * Other values are returned as unparsed JSON. */

#ifndef almost_json_parser_hpp
#define almost_json_parser_hpp

#include <map>
#include <vector>
#include <string>
#include <memory>

namespace almost_json_parser {

constexpr int INPUT_BUFFER_SIZE = 16;

class syntax_error : public std::exception {
public:
    const char *what() const noexcept override;
};

struct parsed_value {
    std::string data;
    bool is_string; // if false, "data" is an unparsed JSON value

    template<typename T> parsed_value(T &&data,bool is_string) :
        data{std::forward<T>(data)}, is_string{is_string} {}
};

class parser;

struct parse_state {
    virtual ~parse_state() = 0;
    virtual bool parse(parser&,const char*&) = 0;
};

using value_map = std::map<std::string,parsed_value>;
using value_array = std::vector<parsed_value>;

class parser {
    char buffer[INPUT_BUFFER_SIZE];
    int buffered;
    std::vector<std::unique_ptr<parse_state>> stack;

public:
    parser() : buffered{0} {}

    void feed(const char *input,size_t size);
    void finish();

    void push_state(parse_state *state);

protected:
    ~parser() {}
    void reset();
};

class object_parser final : public parser {
public:
    value_map value;

    object_parser();

    void reset();
};

class array_parser final : public parser {
public:
    value_array value;

    array_parser();

    void reset();
};

}

#endif

