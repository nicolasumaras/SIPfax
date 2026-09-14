"""Temporary reference corrections for direction and negotiated-value lifetime."""
def apply(source):
    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise ValueError('Pinned negotiation site not unique: ' + old)
        source = source.replace(old, new, 1)
    for remote, local in [('tx', 'rx'), ('rx', 'tx')]:
        for field, default in [('n401', 'V42_DEFAULT_N_401'),
                               ('window_size_k', 'V42_DEFAULT_WINDOW_SIZE_K')]:
            old = f'config.v42_{remote}_{field} =\n                    s->{remote}_{field} = set_param(s->{remote}_{field}, param_val, ss->config.v42_{remote}_{field});'
            new = f'config.v42_{local}_{field} =\n                    s->{local}_{field} = set_param(ss->config.v42_{local}_{field}, param_val, {default});'
            # Mark generated text until all original direction sites are replaced.
            replace(old, new.replace('config.', 'NEGOTIATED_CONFIG.'))
    source = source.replace('NEGOTIATED_CONFIG.', 'config.')
    replace('    memset(&config, 0, sizeof(config));',
            '    memset(&config, 0, sizeof(config));\n'
            '    s->tx_n401 = s->rx_n401 = V42_DEFAULT_N_401;\n'
            '    s->tx_window_size_k = s->rx_window_size_k = V42_DEFAULT_WINDOW_SIZE_K;')
    for direction in ('tx', 'rx'):
        replace(f'put_net_unaligned_uint16(buf, ss->config.v42_{direction}_n401 << 3);',
                f'put_net_unaligned_uint16(buf, (addr == s->rsp_addr ? s->{direction}_n401 : ss->config.v42_{direction}_n401) << 3);')
        replace(f'*buf++ = ss->config.v42_{direction}_window_size_k;',
                f'*buf++ = addr == s->rsp_addr ? s->{direction}_window_size_k : ss->config.v42_{direction}_window_size_k;')
    parameters = '''    s->tx_window_size_k = ss->config.v42_tx_window_size_k;
    s->rx_window_size_k = ss->config.v42_rx_window_size_k;
    s->tx_n401 = ss->config.v42_tx_n401;
    s->rx_n401 = ss->config.v42_rx_n401;'''
    replace(parameters, '')
    helper = '''static void initialize_lapm_parameters(v42_state_t *ss)
{
    lapm_state_t *s = &ss->lapm;
''' + parameters + '\n}\n\n'
    replace('static void reset_lapm(v42_state_t *ss)\n{', helper+'static void reset_lapm(v42_state_t *ss)\n{')
    replace('SPAN_DECLARE(void) v42_restart(v42_state_t *s)\n{',
            'SPAN_DECLARE(void) v42_restart(v42_state_t *s)\n{\n    initialize_lapm_parameters(s);')
    replace('    ss->tx_bit_rate = 28800;\n\n    reset_lapm(ss);',
            '    ss->tx_bit_rate = 28800;\n\n    initialize_lapm_parameters(ss);\n    reset_lapm(ss);')
    return source
