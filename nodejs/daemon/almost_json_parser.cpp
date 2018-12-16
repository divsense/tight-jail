#include <vector>
#include <string.h>
#include <assert.h>

#include "almost_json_parser.hpp"

namespace almost_json_parser {

const char *syntax_error::what() const noexcept override {
    return "syntax error";
}

parse_state::~parse_sate() {}

struct parse_toplevel {
    value_map &value;

    parse_toplevel(value_map &value) : value{value} {}

    bool parse(parser &p,const char *&input) override;
};

struct parse_object {
    value_map &value;
    std::vector<char> entry_name;
    std::vector<char> entry_value;
    bool is_string;
    enum {START,AFTER_NAME,AFTER_COLON,AFTER_VALUE} state;

    parse_object(value_map &value) : value{value}, state{STATRT} {}

    bool parse(parser &p,const char *&input) override;
};

struct parse_string : parse_state {
    std::vector<char> &value;

    parse_string(std::vector<char> &value) : value{value} {}

    bool parse(parser &p,const char *&input) override;
};

struct scan_string : parse_state {
    std::vector<char> &value;

    scan_string(std::vector<char> &value) : value{value} {}

    bool parse(parser &p,const char *&input) override;
};

struct scan_value : parse_state {
    std::vector<char> &value;

    scan_value(std::vector<char> &value) : value{value} {}

    bool parse(parser &p,const char *&input) override;
};

struct scan_balanced : parse_state {
    std::vector<char> &value;
    char closing;

    scan_balanced(std::vector<char> &value,char closing) : value{value}, closing{closing} {}

    bool parse(parser &p,const char *&input) override;
};

bool parse_toplevel::parse(parser &p,const char *&input) {
    switch(*input) {
    case 0:
        return true;
    case ' ':
    case '\t':
    case '\n':
    case '\r':
        ++input;
        break;
    case '{':
        ++input;
        p.push_state(new parse_object(entry_name));
        break;
    default:
        throw syntax_error{};
    }
    return false;
}

bool parse_object::parse(parser &p,const char *&input) {
    switch(state) {
    case START:
        switch(*input) {
        case '}':
            return true;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            ++input;
            break;
        case '"':
            ++input;
            p.push_state(new parse_string(entry_name));
            state = AFTER_NAME;
            break;
        default:
            throw syntax_error{};
        }
        return false;
    case AFTER_NAME:
        switch(*input) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            ++input;
            break;
        case ':':
            ++input;
            state = AFTER_COLON;
            break;
        default:
            throw syntax_error{};
        }
        return false;
    case AFTER_COLON:
        switch(*input) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            ++input;
            break;
        case '"':
            ++input;
            is_string = true;
            p.push_state(new parse_string(entry_value));
            state = AFTER_NAME;
            break;
        default:
            is_string = false;
            p.push_state(new scan_value(entry_value));
            state = AFTER_VALUE;
            break;
        }
        return false;
    default:
        assert(state == AFTER_VALUE);
        value.emplace(
            std::string{entry_name.begin(),entry_name.end()},
            std::string{entry_value.begin(),entry_value.end()},
            is_string);
        entry_name.clear();
        entry_value.clear();
        switch(*input) {
        case '}':
            return true;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            ++input;
            break;
        case ',':
            ++input;
            state = START;
            break;
        default:
            throw syntax_error{};
        }
        return false;
    }
}

bool parse_string::parse(parser &p,const char *&input) {
    switch(*input) {
    case 0:
        throw syntax_error{};
    case '"':
        ++input;
        return true;
    case '\\':
        ++input;
        switch(*input) {
        case '"':
        case '\\':
        case '/':
            value.push_back(*input);
            break;
        case 'b':
            value.push_back('\b');
            break;
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'u':
            {
                int val = 0;
                ++input;
                for(int i=0; i<4; ++i) {
                    char c = *input;
                    val <<= 4;
                    if(c >= '0' && c <= '9') val += c - '0';
                    else if(c >= 'a' && c <= 'f') val += c - 'a' + 10;
                    else if(c >= 'A' && c <= 'F') val += c - 'A' + 10;
                    else throw syntax_error{};
                    ++input;
                }

                // convert to UTF8
                if (val <= 0x7F) {
                    value.push_back(static_cast<char>(val));
                }
                else if(val <= 0x07FF) {
                    value.push_back(static_cast<char>(((val >> 6) & 0x1F) | 0xC0));
                    value.push_back(static_cast<char>((val & 0x3F) | 0x80));
                }
                else if(val <= 0xFFFF) {
                    value.push_back(static_cast<char>(((val >> 12) & 0x0F) | 0xE0));
                    value.push_back(static_cast<char>(((val >> 6) & 0x3F) | 0x80));
                    value.push_back(static_cast<char>((val & 0x3F) | 0x80));
                }
                else if(val <= 0x10FFFF) {
                    value.push_back(static_cast<char>(((val >> 18) & 0x07) | 0xF0));
                    value.push_back(static_cast<char>(((val >> 12) & 0x3F) | 0x80));
                    value.push_back(static_cast<char>(((val >> 6) & 0x3F) | 0x80));
                    value.push_back(static_cast<char>((val & 0x3F) | 0x80));
                }
                else {
                    // error - use replacement character
                    value.push_back(0xEF);
                    value.push_back(0xBF);
                    value.push_back(0xBD);
                }
            }
            break;
        default:
            throw syntax_error{};
        }
        break;
    default:
        value.push_back(*input++);
        break;
    }
    return false;
}

bool scan_string::parse(parser &p,const char *&input) {
    switch(*input) {
    case 0:
        throw syntax_error{};
    case '\\':
        value.push_back(*input++);
        if(!*input) throw syntax_error;
        value.push_back(*input++);
        break;
    case '"':
        value.push_back(*input++);
        return true;
    default:
        value.push_back(*input++);
        break;
    }
    return false;
}

bool scan_value::parse(parser &p,const char *&input) {
    switch(*input) {
    case 0:
    case '}':
    case ']':
    case ')':
    case ',':
        return true;
    case '"':
        value.push_back(*input++);
        p.push_state(new scan_string(value));
        break;
    case '{':
        value.push_back(*input++);
        p.push_state(new scan_balanced(value,'}'));
        break;
    case '[':
        value.push_back(*input++);
        p.push_state(new scan_balanced(value,']'));
        break;
    default:
        value.push_back(*input++);
        break;
    }
    return false;
}

bool scan_balanced::parse(parser &p,const char *&input) {
    if(*input == closing) {
        value.push_back(*input++);
        return true;
    }

    switch(*input) {
    case 0:
    case '}':
    case ']':
    case ')':
        throw syntax_error{};
    case '"':
        value.push_back(*input++);
        p.push_state(new scan_string(value));
        break;
    case '{':
        value.push_back(*input++);
        p.push_state(new scan_balanced(value,'}'));
        break;
    case '[':
        value.push_back(*input++);
        p.push_state(new scan_balanced(value,']'));
        break;
    default:
        value.push_back(*input++);
        break;
    }
    return false;
}

parser::parser() : buffered(0) {
    stack.emplace_back(new parse_toplevel(value));
}

void parser::feed(const char *input,size_t size) {
    assert(buffered < INPUT_BUFFER_SIZE);

    const char *end = input + size;
    while(input < end) {
        buffer[buffered++] = *input++;
        while(buffered == INPUT_BUFFER_SIZE) {
            const char *end = buffer;
            if(stack.back().parse(*this,end)) stack.pop_back();
            buffered -= end - buffer;
            if(buffered) memmove(buffer,end,INPUT_BUFFER_SIZE - buffered);
        }
    }
}

void parser::finish() {
    assert(buffered < INPUT_BUFFER_SIZE);

    buffer[buffered] = 0;
    const char *end = buffer;
    while(stack.size()) {
        if(stack.back().parse(*this,end)) stack.pop_back();
    }
    buffered = 0;
}

void parser::reset() {
    buffered = 0;
    stack.clear();
    value.clear();
    stack.emplace_back(new parse_toplevel(value));
}

void parser::push_state(parse_state *state) {
    stack.emplace_back(state);
}

}


