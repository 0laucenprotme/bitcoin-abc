import mainLogo from 'assets/logo_primary.png';
import tokenLogo from 'assets/logo_secondary.png';
import BigNumber from 'bignumber.js';

export const currency = {
    name: 'eCash',
    ticker: 'XEC',
    logo: mainLogo,
    legacyPrefix: 'bitcoincash',
    prefixes: ['ecash'],
    coingeckoId: 'ecash',
    defaultFee: 2.01,
    dustSats: 550,
    etokenSats: 546,
    cashDecimals: 2,
    chronikUrl: 'https://chronik.fabien.cash',
    blockExplorerUrl: 'https://explorer.be.cash',
    blockExplorerUrlTestnet: 'https://texplorer.bitcoinabc.org',
    pdfReceiptUrl: 'https://blockchair.com/ecash/transaction',
    tokenName: 'eToken',
    tokenTicker: 'eToken',
    tokenIconSubmitApi: 'https://icons.etokens.cash/new',
    tokenLogo: tokenLogo,
    tokenPrefixes: ['etoken'],
    tokenIconsUrl: 'https://etoken-icons.s3.us-west-2.amazonaws.com',
    tokenDbUrl: 'https://tokendb.kingbch.com',
    txHistoryCount: 10,
    xecApiBatchSize: 20,
    websocketDisconnectedRefreshInterval: 1000, // 1000 = 1s
    websocketConnectedRefreshInterval: 30000,
    defaultSettings: { fiatCurrency: 'usd', sendModal: false },
    notificationDurationShort: 3,
    notificationDurationLong: 5,
    localStorageMaxCharacters: 24,
    newTokenDefaultUrl: 'https://cashtab.com/',
    opReturn: {
        opReturnPrefixHex: '6a',
        opReturnPrefixDec: '106',
        opReturnAppPrefixLengthHex: '04',
        opPushDataOne: '4c',
        appPrefixesHex: {
            eToken: '534c5000',
            cashtab: '00746162',
            cashtabEncrypted: '65746162',
            airdrop: '64726f70',
        },
        encryptedMsgCharLimit: 94,
        unencryptedMsgCharLimit: 145,
    },
    settingsValidation: {
        fiatCurrency: [
            'usd',
            'idr',
            'krw',
            'cny',
            'zar',
            'vnd',
            'cad',
            'nok',
            'eur',
            'gbp',
            'jpy',
            'try',
            'rub',
            'inr',
            'brl',
            'php',
            'ils',
            'clp',
            'twd',
            'hkd',
            'bhd',
            'sar',
            'aud',
            'nzd',
            'chf',
        ],
        sendModal: [true, false],
    },
    fiatCurrencies: {
        usd: { name: 'US Dollar', symbol: '$', slug: 'usd' },
        aud: { name: 'Australian Dollar', symbol: '$', slug: 'aud' },
        bhd: { name: 'Bahraini Dinar', symbol: 'BD', slug: 'bhd' },
        brl: { name: 'Brazilian Real', symbol: 'R$', slug: 'brl' },
        gbp: { name: 'British Pound', symbol: '£', slug: 'gbp' },
        cad: { name: 'Canadian Dollar', symbol: '$', slug: 'cad' },
        clp: { name: 'Chilean Peso', symbol: '$', slug: 'clp' },
        cny: { name: 'Chinese Yuan', symbol: '元', slug: 'cny' },
        eur: { name: 'Euro', symbol: '€', slug: 'eur' },
        hkd: { name: 'Hong Kong Dollar', symbol: 'HK$', slug: 'hkd' },
        inr: { name: 'Indian Rupee', symbol: '₹', slug: 'inr' },
        idr: { name: 'Indonesian Rupiah', symbol: 'Rp', slug: 'idr' },
        ils: { name: 'Israeli Shekel', symbol: '₪', slug: 'ils' },
        jpy: { name: 'Japanese Yen', symbol: '¥', slug: 'jpy' },
        krw: { name: 'Korean Won', symbol: '₩', slug: 'krw' },
        nzd: { name: 'New Zealand Dollar', symbol: '$', slug: 'nzd' },
        nok: { name: 'Norwegian Krone', symbol: 'kr', slug: 'nok' },
        php: { name: 'Philippine Peso', symbol: '₱', slug: 'php' },
        rub: { name: 'Russian Ruble', symbol: 'р.', slug: 'rub' },
        twd: { name: 'New Taiwan Dollar', symbol: 'NT$', slug: 'twd' },
        sar: { name: 'Saudi Riyal', symbol: 'SAR', slug: 'sar' },
        zar: { name: 'South African Rand', symbol: 'R', slug: 'zar' },
        chf: { name: 'Swiss Franc', symbol: 'Fr.', slug: 'chf' },
        try: { name: 'Turkish Lira', symbol: '₺', slug: 'try' },
        vnd: { name: 'Vietnamese đồng', symbol: 'đ', slug: 'vnd' },
    },
};

export function parseAddressForParams(addressString) {
    // Build return obj
    const addressInfo = {
        address: '',
        queryString: null,
        amount: null,
    };
    // Parse address string for parameters
    const paramCheck = addressString.split('?');

    let cleanAddress = paramCheck[0];
    addressInfo.address = cleanAddress;

    // Check for parameters
    // only the amount param is currently supported
    let queryString = null;
    let amount = null;
    if (paramCheck.length > 1) {
        queryString = paramCheck[1];
        addressInfo.queryString = queryString;

        const addrParams = new URLSearchParams(queryString);

        if (addrParams.has('amount')) {
            // Amount in XEC
            try {
                amount = new BigNumber(
                    parseFloat(addrParams.get('amount')),
                ).toString();
            } catch (err) {
                amount = null;
            }
        }
    }

    addressInfo.amount = amount;
    return addressInfo;
}
