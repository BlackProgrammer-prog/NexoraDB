"""Lab scan contract; checked against the loaded native constants on connect."""
MAX_PAGE_RECORDS = 1000
DEFAULT_PAGE_BYTES = 4 * 1024**2
MAX_PAGE_BYTES = 64 * 1024**2


def validate_page_size(value):
    if type(value) is not int or not 1 <= value <= MAX_PAGE_RECORDS:
        raise ValueError('scan batch_size must be an integer in 1..1000; load batch_size is separate')


def validate_page_bytes(value):
    if type(value) is not int or not 2 <= value <= MAX_PAGE_BYTES:
        raise ValueError('max_page_bytes must be an integer in 2..64MiB')
