/*
 * brogen.cc
 * Copyright (C) 2015 Mark Adler
 * For conditions of distribution and use, see the accompanying LICENSE file.
 *
 * brogen.cc is a command-driven generator of brotli streams for the purpose of
 * testing brotli decompressors.
 */

#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
using namespace std;

#include <ctype.h>
#include <limits.h>
#include <assert.h>

#include "huff.h"           // Huffman algorithm to make an optimal prefix code
#include "flatten.h"        // Flatten a prefix code to a maximum bit length

// Command numbers for switch statement.
enum command {
    BITS,
    BOUND,
    WBITS,
    LAST,
    META,
    UNCMETA,
    EMPTY,
    LIT,
    TYPES,
    SIMPLE,
    COMPLEX,
    PREFIX,
    HELP,
    UNKNOWN
};

// Map of command names to command numbers.
map <string, enum command> commands() {
    map <string, enum command> m;
    m["b"] = BITS;
    m["bound"] = BOUND;
    m["w"] = WBITS;
    m["last"] = LAST;
    m["m"] = META;
    m["u"] = UNCMETA;
    m["e"] = EMPTY;
    m["lit"] = LIT;
    m["types"] = TYPES;
    m["s"] = SIMPLE;
    m["c"] = COMPLEX;
    m["p"] = PREFIX;
    m["help"] = HELP;
    return m;
};

// Help for each command.
const char help[] =
    "Commands (defaults shown in parentheses):\n"
    "b n x (1 0) - emit the low n bits of x\n"
    "bound x (0) - write the low bits of x to get to a byte boundary\n"
    "w n (16) - Emit the WBITS header for n bits (n in 10..24)\n"
    "last n (1) - The next meta-block is the last one (or not if 'last 0')\n"
    "m n (1) - Compressed Meta-block lead-in with n bytes of data\n"
    "u n (1) - Uncompressed Meta-block lead-in with n bytes of data\n"
    "e n (0) - Empty Meta-block lead-in with n bytes of metadata, or -1\n"
    "          which gives a last empty block with no metadata length\n"
    "lit x x ... - Literal data (numeric bytes and strings)\n"
    "types n (1) - Coded number of block types in 1..256\n"
    "s id t a s s - Simple prefix code type t 1..5, symbols s s ...\n"
    "               alphabet bits a\n"
    "c id b s b s ... - Complex prefix code for symbols s with lengths b\n"
    "p id s s ... - Encode symbols using the prefix code id\n"
    "; - terminates a command (optional)\n"
    "# - starts a comment (ignore the rest of the line)\n"
    "help; - Show this help (semicolon makes it execute immediately)\n";

// Return true if ch could be the first character of a natural number.
int isnum(int ch) {
    return isdigit(ch) || ch == '-' || ch == '+';
}

// Set val to a long integer from stdin, allowing for decimal, hexadecimal, or
// octal notation.  If the next token is not a number, based on the first
// character, then set val to def.  Either way, return 0 for success. If the
// next token appeared to be a number based on the first character, but turned
// out to be invalid, then discard the token, show an error message, and leave
// val unchanged.  In that case, return 1.  In the special case that the number
// ends with a semicolon, consider the number to be valid and push the
// semicolon back on to the stream.
int getlong(long& val, long def = 0) {
    string token;
    long num;
    string::size_type pos;

    cin >> ws;
    if (isnum(cin.peek())) {
        cin >> token;
        try {
            num = stol(token, &pos, 0);
        }
        catch (...) {
            pos = 0;
        }
        if (pos != token.size()) {
            if (token.substr(pos) == ";")
                cin.putback(';');
            else {
                cerr << "! invalid number " << token << " (ignored)\n";
                return 1;
            }
        }
        val = num;
    }
    else
        val = def;
    return 0;
}

// Get literal values from stdin and return as a vector of longs.  Tokens that
// start with a sign (+ or -), a decimal digit, or a double quote consititute a
// literal.  Continue to get literal values and append them to the vector until
// a non-literal token is encountered.  A number, which can be in decimal,
// hexadecimal (starts with 0x after optional sign), or octal (starts with 0
// after optional sign), is appended as single value.  An invalid number is
// discarded with an error message, and processing of literals continues.  A
// double quote starts a string that can include standard C/C++ escapes, and
// that ends with a closing double quote.  The string may contain white space,
// including new lines.  The string characters are each appended as a value to
// the vector.
vector<long> getlit(bool ok) {
    vector<long> vec;
    if (ok) for (;;) {
        cin >> ws;                      // skip whitespace
        long ch = cin.peek();           // peek at 1st character of next token
        if (isnum(ch)) {                // +, -, or digit
            if (getlong(ch) == 0)       // get a number in dec, oct, or hex
                vec.push_back(ch);      // only append to vector if valid
        }
        else if (ch == '"') {           // string
            cin.get();                  // discard double quote
            while (ch = cin.get(), cin && ch != '"') {
                if (ch == '\\') {       // escape
                    ch = cin.get();
                    if (!cin)
                        break;
                    switch (ch) {
                        case 'a':  ch = '\a';  break;
                        case 'b':  ch = '\b';  break;
                        case 'f':  ch = '\f';  break;
                        case 'n':  ch = '\n';  break;
                        case 'r':  ch = '\r';  break;
                        case 't':  ch = '\t';  break;
                        case 'v':  ch = '\v';  break;
                        case 'x':       // hexadecimal
                        {
                            int val = 0;
                            while (ch = cin.peek(), isxdigit(ch))
                                val = (val << 4) + digittoint(cin.get());
                            // no limit on hex digits in C standard (go figure)
                            ch = val & 0xff;
                            break;
                        }
                        default:
                            if (ch >= '0' && ch <= '7') {   // octal
                                int val = digittoint(ch);
                                int count = 1;
                                while (ch = cin.peek(),
                                       ch >= '0' && ch <= '7') {
                                    val = (val << 3) + digittoint(cin.get());
                                    if (++count == 3)
                                        break;  // limit of three octal digits
                                }
                                ch = val & 0xff;
                            }
                            // if not octal, just use character as is, e.g. "
                    }
                }
                vec.push_back(ch);      // append string character to vector
            }
        }
        else
            break;                      // next token is not a literal
    }
    return vec;
}

// Set parm to the first element of vec and remove it from vec, or set parm to
// def if vec is empty.  In either case return zero (success).  If the integer
// is not in the range [low,high], then leave parm unchanged, print an error
// message using the descriptor name, and return non-zero.  Note that if def is
// not in [low,high], then an error is returned if vec is empty.
int getparm(vector<long>& vec, long& parm, long def, long low, long high,
            const char *name) {
    long val;
    if (vec.size()) {
        val = vec.front();
        vec.erase(vec.begin());
    }
    else
        val = def;
    if (val < low || val > high) {
        cerr << "! invalid " << name << " " << val << '\n';
        return 1;
    }
    parm = val;
    return 0;
}

// Output n bits from val.  If n is negative (or there are no arguments), write
// out any remaining bits in the buffer followed by the low bits of val, if
// needed, clearing the buffer.  Between calls there are never more than seven
// bits in the bit buffer.
inline void bout(int n = -1, unsigned val = 0) {
    static unsigned bitbuf = 0;     // unwritten bits
    static int bits = 0;            // number of bits in bitbuf

    // if requested, write out any remaining bits
    if (n < 0) {
        if (bits) {
            bitbuf += val << bits;
            cout << (unsigned char)bitbuf;
            bitbuf = 0;
            bits = 0;
        }
        return;
    }

    // append and write whole bytes
    while (n >= 8) {
        bitbuf += (val & 0xff) << bits;
        val >>= 8;
        n -= 8;
        cout << (unsigned char)bitbuf;
        bitbuf >>= 8;
    }

    // append any remaining bits, write a byte if we have one
    if (n) {
        bitbuf += (val & ((1U << n) - 1)) << bits;
        bits += n;
        if (bits >= 8) {
            cout << (unsigned char)bitbuf;
            bitbuf >>= 8;
            bits -= 8;
        }
    }
}

// Prefix code map type for encoding.
typedef pair <
    unsigned short,     // number of bits in the code (0..15)
    unsigned short      // code in reversed order for ready placement in stream
> code_t;
typedef map <
    unsigned short,     // the key is the symbol value (0..703)
    code_t              // code for that symbol
> prefix_t;

// Create an encoding from a canonical description, assumed to be complete and
// not empty, and where the longest code is assumed to fit in an unsigned
// short.  count[k] is the number of codes with k bits.  *symbol is the list of
// symbols in order from the shortest code to the longest code, sorted by
// symbol value within each code length.  This does not check for repeated
// symbols -- if a symbol is repeated, then only the last (longest) will be
// found when looking up that symbol.
prefix_t encode(unsigned short *count, unsigned short const *symbol) {
    prefix_t encoding;
    unsigned n = 0;
    code_t code(0, 0);
    do {
        while (count[code.first] == 0)
            code.first++;
        encoding[symbol[n++]] = code;
        count[code.first]--;
        unsigned bit = 1U << code.first;    // increment code backwards
        while (bit >>= 1) {
            code.second ^= bit;
            if (code.second & bit)
                break;
        }
    } while (code.second);
    return encoding;
}

// Write a simple code description to cout and return the encoding.  The type
// is in 1..5 and bits is the number of bits required to represent the alphabet
// of symbols (implied by the context in which the code appears).  symbol[] is
// the list of symbols to code, with type symbols unless type is 5, in which
// case there are four symbols.
prefix_t simple(unsigned type, unsigned bits, unsigned short *symbol) {
    // implied number of symbols
    unsigned num = type == 5 ? 4 : type;

    // write out code description
    bout(2, 1);
    bout(2, num - 1);
    for (unsigned n = 0; n < num; n++)
        bout(bits, symbol[n]);
    if (num >= 4)
        bout(1, type - 4);

    // build and return encoding table for this code -- the sorting is required
    // to make the code canonical (the symbols may not be provided in sorted
    // order)
    unsigned short count[4] = {0};
    switch (type) {
        case 1:
            count[0] = 1;
            break;
        case 2:
            count[1] = 2;
            sort(symbol, symbol + 2);
            break;
        case 3:
            count[1] = 1;
            count[2] = 2;
            sort(symbol + 1, symbol + 3);
            break;
        case 4:
            count[2] = 4;
            sort(symbol, symbol + 4);
            break;
        case 5:
            count[1] = 1;
            count[2] = 1;
            count[3] = 2;
            sort(symbol + 2, symbol + 4);
    }
    return encode(count, symbol);
}

// Description of a code, where each pair is a bit length and a symbol value.
typedef pair<unsigned short, unsigned short> sym_t;
typedef vector<sym_t> desc_t;

// Write a complex code description to cout and return the encoding.  The code
// is assumed to be complete with all lengths in the range 1..15.
prefix_t complex(desc_t& desc) {
    // sort by symbols
    sort(desc.begin(), desc.end(),
         [] (sym_t& a, sym_t& b) {
             return a.second < b.second;
         });

    // make a list of instructions to describe the code, making use of
    // run-length encoding where possible
    vector<pair<unsigned char, unsigned char> > inst;
    {
        unsigned rep = 0;       // number of times len repeated
        unsigned len = 0;       // length repeated if rep > 0
        unsigned last = 8;      // last non-zero length emitted

        // function to emit a run of rep len's (brings rep to 0)
        auto emit = [&] () {
            while (rep) {
                if (rep < 3 || len != last) {
                    inst.push_back(make_pair(len, 0));
                    last = len;
                    rep--;
                }
                if (rep >= 3) {
                    // nested coding of repeat of last length
                    unsigned dig[8];        // enough for 15-bit codes
                    unsigned num = 0;
                    rep -= 2;
                    do {
                        dig[num++] = --rep & 3;
                        rep >>= 2;
                    } while (rep);
                    do {
                        inst.push_back(make_pair(16, dig[--num]));
                    } while (num);
                }
            }
        };

        // go through sorted symbols, generating lengths and runs (make use of
        // runs greedily)
        unsigned next = 0;          // next symbol after last encountered
        for (auto& x : desc) {
            // if skipping symbols, then code zeros
            if (next < x.second) {
                emit();             // emit last length run, if any
                auto zeros = x.second - next;
                if (zeros < 3)
                    do {
                        inst.push_back(make_pair(0, 0));
                    } while (--zeros);
                else {
                    // nested codings of repeats of zeros
                    unsigned dig[5];    // enough for 15-bit codes
                    unsigned num = 0;
                    zeros -= 2;
                    do {
                        dig[num++] = --zeros & 7;
                        zeros >>= 3;
                    } while (zeros);
                    do {
                        inst.push_back(make_pair(17, dig[--num]));
                    } while (num);
                }
                next = x.second;
            }

            // accumulate this length, emitting the last one if different
            if (rep && len != x.first)
                emit();             // brings rep to zero
            len = x.first;
            rep++;
            next++;
        }
        emit();                     // emit final length run
    }

    // create a code for the instructions in inst (0..17)
    desc_t instdesc;
    prefix_t instcode;
    {
        // count the occurrences of each instruction
        unsigned short freq[18] = {0};
        for (auto& x : inst)
            freq[x.first]++;

        // make a list of instructions that appear at least once
        for (int n = 0; n < 18; n++)
            if (freq[n])
                instdesc.push_back(make_pair(freq[n], n));

        // make the instructions code
        if (instdesc.size() > 1) {
            // make a Huffman code for the instructions from the frequencies,
            // limiting the longest code to five bits
            sort(instdesc.begin(), instdesc.end()); // sort frequencies
            unsigned syms = 0;
            for (auto& x : instdesc)
                freq[syms++] = x.first;
            huffman(freq, freq, syms);              // in place, freq -> length
            int ret = flatten(freq, syms, 5);       // limit codes to length 5
            assert(ret == 0);
            for (unsigned n = 0; n < syms; n++)
                instdesc[n].first = freq[n];
            sort(instdesc.begin(), instdesc.end()); // canonicalize
            unsigned short count[6] = {0};          // counts for each length
            vector<unsigned short> symbol;          // symbols in length order
            for (auto& x : instdesc) {
                count[x.first]++;
                symbol.push_back(x.second);
            }
            instcode = encode(count, symbol.data());
        }
        else {
            // a single symbol encoded with zero bits
            instdesc[0].first = 3;              // shortest code (that or 4)
            instcode[instdesc[0].second] = make_pair(0, 0);
        }
    }

    // write out the description of the instructions code
    {
        // make the list of lengths to send in permuted order
        unsigned char const order[] = {
            4, 0, 1, 2, 3, 5, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 8, 6
        };
        unsigned char list[18] = {0};
        for (auto& x : instdesc)
            list[order[x.second]] = x.first;

        // determine start and end of list to send -- start becomes the lead-in
        // to the complex code description (0, 2, or 3)
        unsigned start = list[0] || list[1] ? 0 : list[2] ? 2 : 3;
        bout(2, start);
        unsigned end = 17;
        if (instdesc.size() > 1)            // entire list if only one symbol
            while (end && list[end] == 0)
                end--;                      // else drop zeros off the end

        // generate fixed encoding for lengths of instruction codes
        unsigned short count[] = {0, 0, 3, 1, 2};
        unsigned short const symbol[] = {0, 3, 4, 2, 1, 5};
        auto lencode = encode(count, symbol);

        // write out instruction code bit lengths using the code
        for (unsigned n = start; n <= end; n++) {
            auto code = lencode.find(list[n]);
            assert(code != lencode.end());
            bout(code->second.first, code->second.second);
        }
    }

    // write out instructions for the code using the code lengths code
    for (auto& x : inst) {
        auto code = instcode.find(x.first);
        assert(code != instcode.end());
        bout(code->second.first, code->second.second);
        if (x.first > 15)
            bout(x.first - 14, x.second);   // extra bits for 16 or 17
    }

    // generate and return the encoding for this code
    {
        sort(desc.begin(), desc.end());     // canonicalize
        unsigned short count[16] = {0};     // counts for each length
        vector<unsigned short> symbol;      // symbols in length order
        for (auto& x : desc) {
            count[x.first]++;
            symbol.push_back(x.second);
        }
        return encode(count, symbol.data());
    }
}

#define MAXSYMS 704     // symbol values must be in 0..703

// Process commands from stdin, write resulting bit stream to stdout.  Each
// command consists of a command name optionally followed by a series of
// literal values which can be numbers (decimal, hexadecimal, or octal), or
// literal strings.  All white space is equivalent, so multiple commands can be
// given on a line, and command parameters can be broken over several lines. As
// a result, a command will not be executed until the next command or end of
// file is encountered.  If desired, a semicolon can be used to complete a
// command and execute it.  A hash mark (#) starts a comment, which goes to the
// end of that line.
int main() {
    auto decode = commands();               // build map for command decoding
    map <long, prefix_t> codes;             // to save defined prefix codes
    long last = 0;                          // true for the last block
    string token, rest;
    while (token = rest, rest.resize(0), !token.empty() || cin >> token) {
        // handle a start of comment (#) in the token
        {
            auto hash = token.find_first_of('#');
            if (hash < token.size()) {
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                token.resize(hash);
                if (token.empty())
                    continue;
            }
        }

        // handle semicolons in the token
        {
            if (token.front() == ';') {
                rest = token.substr(1);
                continue;
            }
            auto semi = token.find_first_of(';');
            if (semi < token.size()) {
                rest = token.substr(semi);
                token.resize(semi);
            }
        }

        // get command (case-insenstive) and any parameters
        transform(token.begin(), token.end(), token.begin(), ::tolower);
        auto pos = decode.find(token);
        auto cmd = pos == decode.end() ? UNKNOWN : pos->second;
        auto lit = getlit(rest.empty());    // don't bother if semicolon next

        // process command
        long p, q;                          // command parameters
        switch (cmd) {
            case BITS:
                if (getparm(lit, p, 1, 0, LONG_BIT-1, "bits count") |
                    getparm(lit, q, 0, 0, (1 << p) - 1, "bits value"))
                    break;  // (deliberate use of |, to get both parameters)
                bout(p, (unsigned)q);
                break;
            case BOUND:
                p = 0;
                getparm(lit, p, 0, 0, 127, "bound fill bits");
                bout(-1, p);
                break;
            case WBITS:                     // WBITS
                if (getparm(lit, p, 16, 10, 24, "wbits"))
                    break;
                bout(1, p == 16 ? 0 : 1);
                if (p != 16) {
                    bout(3, p < 18 ? 0 : p - 17);
                    if (p < 18)
                        bout(3, p == 17 ? 0 : p - 8);
                }
                break;
            case LAST:                      // set last for next block
                getparm(lit, last, 1, 0, 1, "last");
                break;
            case META:
                if (getparm(lit, p, 1, 1, 1L << 24, "meta-block length"))
                    break;
                if (last)
                    bout(2, 1);             // ISLAST, not empty
                else
                    bout(1, 0);             // not last
                q = p > (1 << 16) ? p > (1 << 20) ? 6 : 5 : 4;
                bout(2, q - 4);             // MNIBBLES (0..2)
                bout(q << 2, p - 1);        // MLEN
                if (!last)
                    bout(1, 0);             // compressed
                break;
            case UNCMETA:
                if (getparm(lit, p, 1, 1, 1L << 24, "meta-block length"))
                    break;
                if (last) {
                    cerr << "last block cannot be uncompressed\n";
                    break;
                }
                bout(1, 0);                 // not last
                q = p > (1 << 16) ? p > (1 << 20) ? 6 : 5 : 4;
                bout(2, q - 4);             // MNIBBLES (0..2)
                bout(q << 2, p - 1);        // MLEN
                bout(1, 1);                 // ISUNCOMPRESSED
                break;
            case EMPTY:
                if (getparm(lit, p, 0, -1, 1L << 24, "meta-data length"))
                    break;
                if (last || p == -1) {
                    if (p == -1) {
                        bout(2, 3);         // ISLAST, ISLASTEMPTY
                        break;
                    }
                    bout(2, 1);             // ISLAST, not empty (though it is)
                }
                else
                    bout(1, 0);             // not last
                bout(2, 3);                 // MNIBBLES: meta-data follows
                bout(1, 0);                 // reserved bit
                q = p > 0 ? p > (1 << 8) ? p > (1L << 16) ? 3 : 2 : 1 : 0;
                bout(2, q);                 // MSKIPBYTES
                bout(q << 3, p - 1);        // MSKIPLEN
                break;
            case LIT:
                bout();                     // go to byte boundary
                for (auto& x : lit)
                    cout << (char)x;        // write out the given bytes
                lit.clear();
                break;
            case TYPES:                     // NBLTYPESx
                if (getparm(lit, p, 1, 1, 256, "number of block types"))
                    break;
                bout(1, p > 1 ? 1 : 0);
                if (p > 1) {
                    q = 0;
                    while ((1 << (q + 1)) < p)
                        q++;
                    bout(3, q);
                    if (q)
                        bout(q, p - 1 - (1 << q));
                }
                break;
            case SIMPLE: {
                long id, type, bits;
                if (getparm(lit, id, 0, LONG_MIN, LONG_MAX, "id") ||
                    getparm(lit, type, 0, 1, 5, "simple code type") ||
                    getparm(lit, bits, 0, 1, 10, "alphabet bits") ||
                    (long)lit.size() != (type == 5 ? 4 : type)) {
                    cerr << "invalid parameters for s -- skipping\n";
                    lit.clear();
                    break;
                }

                // check the list of symbols
                vector<unsigned short> syms;
                {
                    vector<bool> have (MAXSYMS, false);
                    long limit = 1 << bits;
                    if (limit > MAXSYMS)
                        limit = MAXSYMS;
                    bool bad = false;
                    for (auto& x : lit) {
                        if (x < 0 || x >= limit || have[x]) {
                            bad = true;
                            break;
                        }
                        have[x] = true;
                        syms.push_back(x);
                    }
                    if (bad) {
                        cerr << "invalid symbol values -- skipping\n";
                        break;
                    }
                }
                lit.clear();

                // write code description and save encoding
                codes[id] = simple(type, bits, syms.data());
                break;
            }
            case COMPLEX: {
                long id;
                if (getparm(lit, id, 0, LONG_MIN, LONG_MAX, "id") == 0 &&
                    (lit.size() & 1) == 0) {

                    // get code description as length/symbol pairs, check
                    // content
                    desc_t desc;
                    {
                        vector<bool> have (MAXSYMS, false);
                        bool bad = false;
                        for (auto p = lit.begin(); p < lit.end(); p += 2) {
                            long sym = *(p + 1);
                            if (sym < 0 || sym >= MAXSYMS || have[sym]) {
                                bad = true;
                                break;
                            }
                            have[sym] = true;
                            long len = *p;
                            if (len < 0 || len > 15) {
                                bad = true;
                                break;
                            }
                            desc.push_back(make_pair(len, sym));
                        }
                        lit.clear();
                        if (bad) {
                            cerr << "invalid length or symbol values"
                                    " -- skipping\n";
                            break;
                        }
                    }

                    // verify that the code is complete
                    long left = 1 << 15;
                    for (auto& x : desc)
                        left -= 1 << (15 - x.first);
                    if (left) {
                        cerr << "incomplete code -- skipping\n";
                        break;
                    }

                    // write out the code description and return its encoding
                    codes[id] = complex(desc);
                }
                else {
                    cerr << "invalid code id or missing symbol -- skipping\n";
                    lit.clear();
                }
                break;
            }
            case PREFIX: {
                long id;
                if (getparm(lit, id, 0, LONG_MIN, LONG_MAX, "id") == 0) {
                    auto encoding = codes.find(id);
                    if (encoding != codes.end()) {
                        for (auto& sym : lit) {
                            auto code = encoding->second.find(sym);
                            if (code != encoding->second.end())
                                bout(code->second.first, code->second.second);
                            else
                                cerr << "symbol " << sym <<
                                "not found in code " << id << "\n";
                        }
                    }
                    else
                        cerr << "code " << id << "not found\n";
                }
                else
                    cerr << "invalid code id for p -- skipping\n";
                lit.clear();
                break;
            }
            case HELP:
                cerr << help;
                break;
            case UNKNOWN:
            default:
                cerr << "! unknown command: " << token << '\n';
        }
        if (lit.size())
            cerr << lit.size() << " extraneous parameters for " <<
                    token << " ignored\n";
    }
    bout();                                 // flush out the last bits, if any
}
