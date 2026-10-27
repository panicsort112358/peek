#define PEEK_COMPILED 0
#include "../peek.hpp"

int main()
{
    int side_effects = 0;
    PEEK(++side_effects, a_name_that_does_not_exist);
    PEEK_IF(++side_effects, another_missing_name);
    PEEK_SCOPE(yet_another_missing_name);
    return side_effects;
}
