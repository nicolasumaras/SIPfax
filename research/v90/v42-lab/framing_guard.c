/* Validate the complete XID envelope before the reference mutates parameters. */
static int validated_xid(const uint8_t *frame, int len)
{
    unsigned position = 3;
    if (len < 3 || frame[2] != FI_GENERAL)
        return 0;
    while (position < (unsigned) len)
    {
        unsigned group = frame[position++];
        unsigned length;
        if (group == GI_USER_DATA)
        {
            length = (unsigned) len - position;
        }
        else
        {
            if ((unsigned) len - position < 2)
                return 0;
            length = ((unsigned) frame[position] << 8) | frame[position + 1];
            position += 2;
        }
        if (length > (unsigned) len - position)
            return 0;
        unsigned end = position + length;
        if (group == GI_PARAM_NEGOTIATION || group == GI_PRIVATE_NEGOTIATION
            || group == GI_USER_DATA)
        {
            while (position < end)
            {
                if (end - position < 2)
                    return 0;
                unsigned parameter_length = frame[position + 1];
                position += 2;
                if (parameter_length > end - position)
                    return 0;
                position += parameter_length;
            }
        }
        position = end;
    }
    return 1;
}
