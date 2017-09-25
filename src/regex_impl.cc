#include "regex_impl.hh"
#include "vector.hh"
#include "unit_tests.hh"
#include "string.hh"
#include "unicode.hh"
#include "utf8.hh"
#include "utf8_iterator.hh"
#include "exception.hh"
#include "array_view.hh"

namespace Kakoune
{

struct CompiledRegex
{
    enum Op : char
    {
        Match,
        Literal,
        AnyChar,
        CharRange,
        NegativeCharRange,
        Jump,
        Split_PrioritizeParent,
        Split_PrioritizeChild,
        Save,
        LineStart,
        LineEnd,
        WordBoundary,
        NotWordBoundary,
        SubjectBegin,
        SubjectEnd,
    };

    using Offset = unsigned;

    Vector<char> bytecode;
    size_t save_count;
};

namespace RegexCompiler
{

struct Quantifier
{
    enum Type
    {
        One,
        Optional,
        RepeatZeroOrMore,
        RepeatOneOrMore,
        RepeatMinMax,
    };
    Type type = One;
    int min = -1, max = -1;

    bool allows_none() const
    {
        return type == Quantifier::Optional or
               type == Quantifier::RepeatZeroOrMore or
              (type == Quantifier::RepeatMinMax and min <= 0);
    }

    bool allows_infinite_repeat() const
    {
        return type == Quantifier::RepeatZeroOrMore or
               type == Quantifier::RepeatOneOrMore or
              (type == Quantifier::RepeatMinMax and max == -1);
    };
};

enum class Op
{
    Literal,
    AnyChar,
    CharRange,
    NegativeCharRange,
    Sequence,
    Alternation,
    LineStart,
    LineEnd,
    WordBoundary,
    NotWordBoundary,
    SubjectBegin,
    SubjectEnd,
};

struct AstNode
{
    Op op;
    Codepoint value;
    Quantifier quantifier;
    Vector<std::unique_ptr<AstNode>> children;
};

using AstNodePtr = std::unique_ptr<AstNode>;

struct CharRange { Codepoint min, max; };

struct ParsedRegex
{
    AstNodePtr ast;
    size_t capture_count;
    Vector<Vector<CharRange>> ranges;
};

AstNodePtr make_ast_node(Op op, Codepoint value = -1,
                         Quantifier quantifier = {Quantifier::One})
{
    return AstNodePtr{new AstNode{op, value, quantifier, {}}};
}

// Recursive descent parser based on naming used in the ECMAScript
// standard, although the syntax is not fully compatible.
struct Parser
{
    struct InvalidPolicy
    {
        Codepoint operator()(Codepoint cp) { throw runtime_error{"Invalid utf8 in regex"}; }
    };

    using Iterator = utf8::iterator<const char*, Codepoint, int, InvalidPolicy>;

    static ParsedRegex parse(StringView re)
    {
        ParsedRegex res;
        res.capture_count = 1;
        Iterator pos{re.begin(), re}, end{re.end(), re};
        res.ast = disjunction(res, pos, end, 0);
        return res;
    }

private:
    static AstNodePtr disjunction(ParsedRegex& parsed_regex, Iterator& pos, Iterator end, unsigned capture = -1)
    {
        AstNodePtr node = alternative(parsed_regex, pos, end);
        if (pos == end or *pos != '|')
        {
            node->value = capture;
            return node;
        }

        AstNodePtr res = make_ast_node(Op::Alternation);
        res->children.push_back(std::move(node));
        res->children.push_back(disjunction(parsed_regex, ++pos, end));
        res->value = capture;
        return res;
    }

    static AstNodePtr alternative(ParsedRegex& parsed_regex, Iterator& pos, Iterator end)
    {
        AstNodePtr res = make_ast_node(Op::Sequence);
        while (auto node = term(parsed_regex, pos, end))
            res->children.push_back(std::move(node));
        if (res->children.empty())
            throw runtime_error{"Parse error in alternative"};
        return res;
    }

    static AstNodePtr term(ParsedRegex& parsed_regex, Iterator& pos, Iterator end)
    {
        if (auto node = assertion(parsed_regex, pos, end))
            return node;
        if (auto node = atom(parsed_regex, pos, end))
        {
            node->quantifier = quantifier(parsed_regex, pos, end);
            return node;
        }
        return nullptr;
    }

    static AstNodePtr assertion(ParsedRegex& parsed_regex, Iterator& pos, Iterator end)
    {
        if (pos == end)
            return nullptr;

        switch (*pos)
        {
            case '^': ++pos; return make_ast_node(Op::LineStart);
            case '$': ++pos; return make_ast_node(Op::LineEnd);
            case '\\':
                if (pos+1 == end)
                    return nullptr;
                switch (*(pos+1))
                {
                    case 'b': pos += 2; return make_ast_node(Op::WordBoundary);
                    case 'B': pos += 2; return make_ast_node(Op::NotWordBoundary);
                    case '`': pos += 2; return make_ast_node(Op::SubjectBegin);
                    case '\'': pos += 2; return make_ast_node(Op::SubjectEnd);
                }
                break;
            /* TODO: look ahead, look behind */
        }
        return nullptr;
    }

    static AstNodePtr atom(ParsedRegex& parsed_regex, Iterator& pos, Iterator end)
    {
        if (pos == end)
            return nullptr;

        const Codepoint cp = *pos;
        switch (cp)
        {
            case '.': ++pos; return make_ast_node(Op::AnyChar);
            case '(':
            {
                ++pos;
                auto content = disjunction(parsed_regex, pos, end, parsed_regex.capture_count++);

                if (pos == end or *pos != ')')
                    throw runtime_error{"Unclosed parenthesis"};
                ++pos;
                return content;
            }
            case '\\':
                ++pos;
                return atom_escape(parsed_regex, pos, end);
            case '[':
                ++pos;
                return character_class(parsed_regex, pos, end);
            default:
                if (contains("^$.*+?()[]{}|", cp))
                    return nullptr;
                ++pos;
                return make_ast_node(Op::Literal, cp);
        }
    }

    static AstNodePtr atom_escape(ParsedRegex& parsed_regex, Iterator& pos, Iterator end)
    {
        const Codepoint cp = *pos;

        struct { Codepoint name; Codepoint value; } control_escapes[] = {
            { 'f', '\f' }, { 'n', '\n' }, { 'r', '\r' }, { 't', '\t' }, { 'v', '\v' }
        };
        for (auto& control : control_escapes)
        {
            if (control.name == cp)
                return make_ast_node(Op::Literal, control.value);
        }

        // TOOD: \c..., \0..., '\0x...', \u...

        if (contains("^$\\.*+?()[]{}|", cp)) // SyntaxCharacter
            return make_ast_node(Op::Literal, cp);
        throw runtime_error{"Unknown atom escape"};
    }

    static AstNodePtr character_class(ParsedRegex& parsed_regex, Iterator& pos, Iterator end)
    {
        const bool negative = pos != end and *pos == '^';
        if (negative)
            ++pos;

        Vector<CharRange> ranges;
        while (pos != end and *pos != ']')
        {
            const auto cp = *pos++;
            if (cp == '-')
            {
                ranges.push_back({ '-', 0 });
                continue;
            }

            if (pos == end)
                break;

            CharRange range = { cp, 0 };
            if (*pos == '-')
            {
                if (++pos == end)
                    break;
                range.max = *pos++;
                if (range.min > range.max)
                    throw runtime_error{"Invalid range specified"};
            }
            ranges.push_back(range);
        }
        if (pos == end)
            throw runtime_error{"Unclosed character class"};
        ++pos;

        auto ranges_id = parsed_regex.ranges.size();
        parsed_regex.ranges.push_back(std::move(ranges));

        return make_ast_node(negative ? Op::NegativeCharRange : Op::CharRange, ranges_id);
    }

    static Quantifier quantifier(ParsedRegex& parsed_regex, Iterator& pos, Iterator end)
    {
        if (pos == end)
            return {Quantifier::One};

        auto read_int = [](Iterator& pos, Iterator begin, Iterator end) {
            int res = 0;
            for (; pos != end; ++pos)
            {
                const auto cp = *pos;
                if (cp < '0' or cp > '9')
                    return pos == begin ? -1 : res;
                res = res * 10 + cp - '0';
            }
            return res;
        };

        switch (*pos)
        {
            case '*': ++pos; return {Quantifier::RepeatZeroOrMore};
            case '+': ++pos; return {Quantifier::RepeatOneOrMore};
            case '?': ++pos; return {Quantifier::Optional};
            case '{':
            {
                auto it = pos+1;
                int min = read_int(it, it, end);
                int max = -1;
                if (*it == ',')
                {
                    ++it;
                    max = read_int(it, it, end);
                }
                if (*it++ != '}')
                   throw runtime_error{"expected closing bracket"};
                pos = it;
                return {Quantifier::RepeatMinMax, min, max};
            }
            default: return {Quantifier::One};
        }
    }
};

CompiledRegex::Offset alloc_offset(CompiledRegex& program)
{
    auto pos = program.bytecode.size();
    program.bytecode.resize(pos + sizeof(CompiledRegex::Offset));
    return pos;
}

CompiledRegex::Offset& get_offset(CompiledRegex& program, CompiledRegex::Offset pos)
{
    return *reinterpret_cast<CompiledRegex::Offset*>(&program.bytecode[pos]);
}

void push_codepoint(CompiledRegex& program, Codepoint cp)
{
    utf8::dump(std::back_inserter(program.bytecode), cp);
}

CompiledRegex::Offset compile_node(CompiledRegex& program, const ParsedRegex& parsed_regex, const AstNodePtr& node);

CompiledRegex::Offset compile_node_inner(CompiledRegex& program, const ParsedRegex& parsed_regex, const AstNodePtr& node)
{
    const auto start_pos = program.bytecode.size();

    const Codepoint capture = (node->op == Op::Alternation or node->op == Op::Sequence) ? node->value : -1;
    if (capture != -1)
    {
        program.bytecode.push_back(CompiledRegex::Save);
        program.bytecode.push_back(capture * 2);
    }

    Vector<CompiledRegex::Offset> goto_inner_end_offsets;
    switch (node->op)
    {
        case Op::Literal:
            program.bytecode.push_back(CompiledRegex::Literal);
            push_codepoint(program, node->value);
            break;
        case Op::AnyChar:
            program.bytecode.push_back(CompiledRegex::AnyChar);
            break;
        case Op::CharRange: case Op::NegativeCharRange:
        {
            auto& ranges = parsed_regex.ranges[node->value];
            size_t single_count = std::count_if(ranges.begin(), ranges.end(),
                                                [](auto& r) { return r.max == 0; });
            program.bytecode.push_back(node->op == Op::CharRange ?
                                           CompiledRegex::CharRange
                                         : CompiledRegex::NegativeCharRange);

            program.bytecode.push_back((char)single_count);
            program.bytecode.push_back((char)(ranges.size() - single_count));
            for (auto& r : ranges)
            {
                if (r.max == 0)
                    push_codepoint(program, r.min);
            }
            for (auto& r : ranges)
            {
                if (r.max != 0)
                {
                    push_codepoint(program, r.min);
                    push_codepoint(program, r.max);
                }
            }
            break;
        }
        case Op::Sequence:
            for (auto& child : node->children)
                compile_node(program, parsed_regex, child);
            break;
        case Op::Alternation:
        {
            auto& children = node->children;
            kak_assert(children.size() == 2);

            program.bytecode.push_back(CompiledRegex::Split_PrioritizeParent);
            auto offset = alloc_offset(program);

            compile_node(program, parsed_regex, children[0]);
            program.bytecode.push_back(CompiledRegex::Jump);
            goto_inner_end_offsets.push_back(alloc_offset(program));

            auto right_pos = compile_node(program, parsed_regex, children[1]);
            get_offset(program, offset) = right_pos;

            break;
        }
        case Op::LineStart:
            program.bytecode.push_back(CompiledRegex::LineStart);
            break;
        case Op::LineEnd:
            program.bytecode.push_back(CompiledRegex::LineEnd);
            break;
        case Op::WordBoundary:
            program.bytecode.push_back(CompiledRegex::WordBoundary);
            break;
        case Op::NotWordBoundary:
            program.bytecode.push_back(CompiledRegex::NotWordBoundary);
            break;
        case Op::SubjectBegin:
            program.bytecode.push_back(CompiledRegex::SubjectBegin);
            break;
        case Op::SubjectEnd:
            program.bytecode.push_back(CompiledRegex::SubjectEnd);
            break;
    }

    for (auto& offset : goto_inner_end_offsets)
        get_offset(program, offset) =  program.bytecode.size();

    if (capture != -1)
    {
        program.bytecode.push_back(CompiledRegex::Save);
        program.bytecode.push_back(capture * 2 + 1);
    }

    return start_pos;
}

CompiledRegex::Offset compile_node(CompiledRegex& program, const ParsedRegex& parsed_regex, const AstNodePtr& node)
{
    CompiledRegex::Offset pos = program.bytecode.size();
    Vector<CompiledRegex::Offset> goto_end_offsets;

    if (node->quantifier.allows_none())
    {
        program.bytecode.push_back(CompiledRegex::Split_PrioritizeParent);
        goto_end_offsets.push_back(alloc_offset(program));
    }

    auto inner_pos = compile_node_inner(program, parsed_regex, node);
    // Write the node multiple times when we have a min count quantifier
    for (int i = 1; i < node->quantifier.min; ++i)
        inner_pos = compile_node_inner(program, parsed_regex, node);

    if (node->quantifier.allows_infinite_repeat())
    {
        program.bytecode.push_back(CompiledRegex::Split_PrioritizeChild);
        get_offset(program, alloc_offset(program)) = inner_pos;
    }
    // Write the node as an optional match for the min -> max counts
    else for (int i = std::max(1, node->quantifier.min); // STILL UGLY !
              i < node->quantifier.max; ++i)
    {
        program.bytecode.push_back(CompiledRegex::Split_PrioritizeParent);
        goto_end_offsets.push_back(alloc_offset(program));
        compile_node_inner(program, parsed_regex, node);
    }

    for (auto offset : goto_end_offsets)
        get_offset(program, offset) = program.bytecode.size();

    return pos;
}

constexpr CompiledRegex::Offset prefix_size = 3 + 2 * sizeof(CompiledRegex::Offset);

// Add a '.*' as the first instructions for the search use case
void write_search_prefix(CompiledRegex& program)
{
    kak_assert(program.bytecode.empty());
    program.bytecode.push_back(CompiledRegex::Split_PrioritizeChild);
    get_offset(program, alloc_offset(program)) = prefix_size;
    program.bytecode.push_back(CompiledRegex::AnyChar);
    program.bytecode.push_back(CompiledRegex::Split_PrioritizeParent);
    get_offset(program, alloc_offset(program)) = 1 + sizeof(CompiledRegex::Offset);
}

CompiledRegex compile(const ParsedRegex& parsed_regex)
{
    CompiledRegex res;
    write_search_prefix(res);
    compile_node(res, parsed_regex, parsed_regex.ast);
    res.bytecode.push_back(CompiledRegex::Match);
    res.save_count = parsed_regex.capture_count * 2;
    return res;
}

CompiledRegex compile(StringView re)
{
    return compile(Parser::parse(re));
}

}

void dump(const CompiledRegex& program)
{
    for (auto pos = program.bytecode.data(), end = program.bytecode.data() + program.bytecode.size();
         pos < end; )
    {
        printf("%4zd    ", pos - program.bytecode.data());
        const auto op = (CompiledRegex::Op)*pos++;
        switch (op)
        {
            case CompiledRegex::Literal:
                printf("literal %lc\n", utf8::read_codepoint(pos, (const char*)nullptr));
                break;
            case CompiledRegex::AnyChar:
                printf("any char\n");
                break;
            case CompiledRegex::Jump:
                printf("jump %u\n", *reinterpret_cast<const CompiledRegex::Offset*>(&*pos));
                pos += sizeof(CompiledRegex::Offset);
                break;
            case CompiledRegex::Split_PrioritizeParent:
            case CompiledRegex::Split_PrioritizeChild:
            {
                printf("split (prioritize %s) %u\n",
                       op == CompiledRegex::Split_PrioritizeParent ? "parent" : "child",
                       *reinterpret_cast<const CompiledRegex::Offset*>(&*pos));
                pos += sizeof(CompiledRegex::Offset);
                break;
            }
            case CompiledRegex::Save:
                printf("save %d\n", *pos++);
                break;
            case CompiledRegex::CharRange: case CompiledRegex::NegativeCharRange:
            {
                printf("%schar range, [", op == CompiledRegex::NegativeCharRange ? "negative " : "");
                auto single_count = *pos++;
                auto range_count = *pos++;
                for (int i = 0; i < single_count; ++i)
                    printf("%lc", utf8::read_codepoint(pos, (const char*)nullptr));
                printf("]");

                for (int i = 0; i < range_count; ++i)
                {
                    Codepoint min = utf8::read_codepoint(pos, (const char*)nullptr);
                    Codepoint max = utf8::read_codepoint(pos, (const char*)nullptr);
                    printf(" [%lc-%lc]", min, max);
                }
                printf("\n");
                break;
            }
            case CompiledRegex::LineStart:
                printf("line start\n");
                break;
            case CompiledRegex::LineEnd:
                printf("line end\n");
                break;
            case CompiledRegex::WordBoundary:
                printf("word boundary\n");
                break;
            case CompiledRegex::NotWordBoundary:
                printf("not word boundary\n");
                break;
            case CompiledRegex::SubjectBegin:
                printf("subject begin\n");
                break;
            case CompiledRegex::SubjectEnd:
                printf("subject end\n");
                break;
            case CompiledRegex::Match:
                printf("match\n");
        }
    }
}

template<typename Iterator>
struct ThreadedRegexVM
{
    ThreadedRegexVM(const CompiledRegex& program)
      : m_program{program} {}

    struct Thread
    {
        const char* inst;
        Vector<const char*> saves = {};
    };

    enum class StepResult { Consumed, Matched, Failed };
    StepResult step(size_t thread_index)
    {
        const auto prog_start = m_program.bytecode.data();
        const auto prog_end = prog_start + m_program.bytecode.size();
        while (true)
        {
            auto& thread = m_threads[thread_index];
            const Codepoint cp = m_pos == m_end ? 0 : *m_pos;
            const CompiledRegex::Op op = (CompiledRegex::Op)*thread.inst++;
            switch (op)
            {
                case CompiledRegex::Literal:
                    if (utf8::read_codepoint(thread.inst, prog_end) == cp)
                        return StepResult::Consumed;
                    return StepResult::Failed;
                case CompiledRegex::AnyChar:
                    return StepResult::Consumed;
                case CompiledRegex::Jump:
                {
                    auto inst = prog_start + *reinterpret_cast<const CompiledRegex::Offset*>(thread.inst);
                    // if instruction is already going to be executed by another thread, drop this thread
                    if (std::find_if(m_threads.begin(), m_threads.end(),
                                     [inst](const Thread& t) { return t.inst == inst; }) != m_threads.end())
                        return StepResult::Failed;
                    thread.inst = inst;
                    break;
                }
                case CompiledRegex::Split_PrioritizeParent:
                {
                    add_thread(thread_index+1, *reinterpret_cast<const CompiledRegex::Offset*>(thread.inst), thread.saves);
                    // thread is invalidated now, as we mutated the m_thread vector
                    m_threads[thread_index].inst += sizeof(CompiledRegex::Offset);
                    break;
                }
                case CompiledRegex::Split_PrioritizeChild:
                {
                    add_thread(thread_index+1, thread.inst + sizeof(CompiledRegex::Offset) - prog_start, thread.saves);
                    // thread is invalidated now, as we mutated the m_thread vector
                    m_threads[thread_index].inst = prog_start + *reinterpret_cast<const CompiledRegex::Offset*>(m_threads[thread_index].inst);
                    break;
                }
                case CompiledRegex::Save:
                {
                    const char index = *thread.inst++;
                    thread.saves[index] = m_pos.base();
                    break;
                }
                case CompiledRegex::CharRange: case CompiledRegex::NegativeCharRange:
                {
                    const int single_count = *thread.inst++;
                    const int range_count = *thread.inst++;
                    for (int i = 0; i < single_count; ++i)
                    {
                        auto candidate = utf8::read_codepoint(thread.inst, prog_end);
                        if (cp == candidate)
                        {
                            thread.inst = utf8::advance(thread.inst, prog_end, CharCount{single_count - (i + 1) + range_count * 2});
                            return op == CompiledRegex::CharRange ? StepResult::Consumed : StepResult::Failed;
                        }
                    }
                    for (int i = 0; i < range_count; ++i)
                    {
                        auto min = utf8::read_codepoint(thread.inst, prog_end);
                        auto max = utf8::read_codepoint(thread.inst, prog_end);
                        if (min <= cp and cp <= max)
                        {
                            thread.inst = utf8::advance(thread.inst, prog_end, CharCount{(range_count - (i + 1)) * 2});
                            return op == CompiledRegex::CharRange ? StepResult::Consumed : StepResult::Failed;
                        }
                    }
                    return op == CompiledRegex::CharRange ? StepResult::Failed : StepResult::Consumed;
                }
                case CompiledRegex::LineStart:
                    if (not is_line_start())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::LineEnd:
                    if (not is_line_end())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::WordBoundary:
                    if (not is_word_boundary())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::NotWordBoundary:
                    if (is_word_boundary())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::SubjectBegin:
                    if (m_pos != m_begin)
                        return StepResult::Failed;
                    break;
                case CompiledRegex::SubjectEnd:
                    if (m_pos != m_end)
                        return StepResult::Failed;
                    break;
                case CompiledRegex::Match:
                    thread.inst = nullptr;
                    return StepResult::Matched;
            }
        }
        return StepResult::Failed;
    }

    bool exec(StringView data, bool match = true, bool longest = false)
    {
        bool found_match = false;
        m_threads.clear();
        add_thread(0, match ? RegexCompiler::prefix_size : 0,
                   Vector<const char*>(m_program.save_count, nullptr));

        m_begin = data.begin();
        m_end = data.end();

        for (m_pos = Utf8It{m_begin, m_begin, m_end}; m_pos != m_end; ++m_pos)
        {
            for (int i = 0; i < m_threads.size(); ++i)
            {
                const auto res = step(i);
                if (res == StepResult::Matched)
                {
                    if (match)
                        continue; // We are not at end, this is not a full match

                    m_captures = std::move(m_threads[i].saves);
                    found_match = true;
                    m_threads.resize(i); // remove this and lower priority threads
                    if (not longest)
                        return true;
                }
                else if (res == StepResult::Failed)
                    m_threads[i].inst = nullptr;
            }
            m_threads.erase(std::remove_if(m_threads.begin(), m_threads.end(),
                                           [](const Thread& t) { return t.inst == nullptr; }), m_threads.end());
            if (m_threads.empty())
                return false;
        }

        // Step remaining threads to see if they match without consuming anything else
        for (int i = 0; i < m_threads.size(); ++i)
        {
            if (step(i) == StepResult::Matched)
            {
                m_captures = std::move(m_threads[i].saves);
                found_match = true;
                m_threads.resize(i); // remove this and lower priority threads
                if (not longest)
                    return true;
            }
        }
        return found_match;
    }

    void add_thread(int index, CompiledRegex::Offset pos, Vector<const char*> saves)
    {
        const char* inst = m_program.bytecode.data() + pos;
        if (std::find_if(m_threads.begin(), m_threads.end(),
                         [inst](const Thread& t) { return t.inst == inst; }) == m_threads.end())
            m_threads.insert(m_threads.begin() + index, {inst, std::move(saves)});
    }

    bool is_line_start() const
    {
        return m_pos == m_begin or *(m_pos-1) == '\n';
    }

    bool is_line_end() const
    {
        return m_pos == m_end or *m_pos == '\n';
    }

    bool is_word_boundary() const
    {
        return m_pos == m_begin or m_pos == m_end or
               is_word(*(m_pos-1)) != is_word(*m_pos);
    }

    const CompiledRegex& m_program;
    Vector<Thread> m_threads;

    using Utf8It = utf8::iterator<Iterator>;

    Iterator m_begin;
    Iterator m_end;
    Utf8It m_pos;

    Vector<const char*> m_captures;
};

auto test_regex = UnitTest{[]{
    {
        auto program = RegexCompiler::compile(R"(a*b)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("b"));
        kak_assert(vm.exec("ab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("acb"));
        kak_assert(not vm.exec("abc"));
        kak_assert(not vm.exec(""));
    }

    {
        auto program = RegexCompiler::compile(R"(^a.*b$)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("afoob"));
        kak_assert(vm.exec("ab"));
        kak_assert(not vm.exec("bab"));
        kak_assert(not vm.exec(""));
    }

    {
        auto program = RegexCompiler::compile(R"(^(foo|qux|baz)+(bar)?baz$)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("fooquxbarbaz"));
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == "qux");
        kak_assert(not vm.exec("fooquxbarbaze"));
        kak_assert(not vm.exec("quxbar"));
        kak_assert(not vm.exec("blahblah"));
        kak_assert(vm.exec("bazbaz"));
        kak_assert(vm.exec("quxbaz"));
    }

    {
        auto program = RegexCompiler::compile(R"(.*\b(foo|bar)\b.*)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("qux foo baz"));
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == "foo");
        kak_assert(not vm.exec("quxfoobaz"));
        kak_assert(vm.exec("bar"));
        kak_assert(not vm.exec("foobar"));
    }
    {
        auto program = RegexCompiler::compile(R"(\`(foo|bar)\')");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("foo"));
        kak_assert(vm.exec("bar"));
        kak_assert(not vm.exec("foobar"));
    }

    {
        auto program = RegexCompiler::compile(R"(\`a{3,5}b\')");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaaaab"));
        kak_assert(vm.exec("aaaaab"));
    }

    {
        auto program = RegexCompiler::compile(R"(\`a{3,}b\')");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(vm.exec("aaaaab"));
    }

    {
        auto program = RegexCompiler::compile(R"(\`a{,3}b\')");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("b"));
        kak_assert(vm.exec("ab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaab"));
    }

    {
        auto program = RegexCompiler::compile(R"(f.*a(.*o))");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("blahfoobarfoobaz", false, true));
        kak_assert(StringView{vm.m_captures[0], vm.m_captures[1]} == "foobarfoo");
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == "rfoo");
        kak_assert(vm.exec("mais que fais la police", false, true));
        kak_assert(StringView{vm.m_captures[0], vm.m_captures[1]} == "fais la po");
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == " po");
    }

    {
        auto program = RegexCompiler::compile(R"([àb-dX-Z]{3,5})");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("càY"));
        kak_assert(not vm.exec("àeY"));
        kak_assert(vm.exec("dcbàX"));
        kak_assert(not vm.exec("efg"));
    }
}};

}