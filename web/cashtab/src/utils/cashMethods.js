import { currency } from 'components/Common/Ticker';
import {
    isValidXecAddress,
    isValidEtokenAddress,
    isValidBchApiUtxoObject,
    isValidContactList,
} from 'utils/validation';
import BigNumber from 'bignumber.js';
import cashaddr from 'ecashaddrjs';
import useBCH from '../hooks/useBCH';

export const generateTxInput = (
    BCH,
    isOneToMany,
    utxos,
    txBuilder,
    destinationAddressAndValueArray,
    satoshisToSend,
    feeInSatsPerByte,
) => {
    const { calcFee } = useBCH();
    let txInputObj = {};
    const inputUtxos = [];
    let txFee = 0;
    let totalInputUtxoValue = new BigNumber(0);
    try {
        if (
            !BCH ||
            (isOneToMany && !destinationAddressAndValueArray) ||
            !utxos ||
            !txBuilder ||
            !satoshisToSend ||
            !feeInSatsPerByte
        ) {
            throw new Error('Invalid tx input parameter');
        }

        // A normal tx will have 2 outputs, destination and change
        // A one to many tx will have n outputs + 1 change output, where n is the number of recipients
        const txOutputs = isOneToMany
            ? destinationAddressAndValueArray.length + 1
            : 2;
        for (let i = 0; i < utxos.length; i++) {
            const utxo = utxos[i];
            totalInputUtxoValue = totalInputUtxoValue.plus(utxo.value);
            const vout = utxo.vout;
            const txid = utxo.txid;
            // add input with txid and index of vout
            txBuilder.addInput(txid, vout);

            inputUtxos.push(utxo);
            txFee = calcFee(BCH, inputUtxos, txOutputs, feeInSatsPerByte);

            if (totalInputUtxoValue.minus(satoshisToSend).minus(txFee).gte(0)) {
                break;
            }
        }
    } catch (err) {
        console.log(`generateTxInput() error: ` + err);
        throw err;
    }
    txInputObj.txBuilder = txBuilder;
    txInputObj.totalInputUtxoValue = totalInputUtxoValue;
    txInputObj.inputUtxos = inputUtxos;
    txInputObj.txFee = txFee;
    return txInputObj;
};

export const getChangeAddressFromInputUtxos = (BCH, inputUtxos, wallet) => {
    if (!BCH || !inputUtxos || !wallet) {
        throw new Error('Invalid getChangeAddressFromWallet input parameter');
    }

    // Assume change address is input address of utxo at index 0
    let changeAddress;

    // Validate address
    try {
        changeAddress = inputUtxos[0].address;
        BCH.Address.isCashAddress(changeAddress);
    } catch (err) {
        throw new Error('Invalid input utxo');
    }
    return changeAddress;
};

/*
 * Parse the total value of a send XEC tx and checks whether it is more than dust
 * One to many: isOneToMany is true, singleSendValue is null
 * One to one: isOneToMany is false, destinationAddressAndValueArray is null
 * Returns the aggregate send value in BigNumber format
 */
export const parseXecSendValue = (
    isOneToMany,
    singleSendValue,
    destinationAddressAndValueArray,
) => {
    let value = new BigNumber(0);

    try {
        if (isOneToMany) {
            // this is a one to many XEC transaction
            if (
                !destinationAddressAndValueArray ||
                !destinationAddressAndValueArray.length
            ) {
                throw new Error('Invalid destinationAddressAndValueArray');
            }
            const arrayLength = destinationAddressAndValueArray.length;
            for (let i = 0; i < arrayLength; i++) {
                // add the total value being sent in this array of recipients
                // each array row is: 'eCash address, send value'
                value = BigNumber.sum(
                    value,
                    new BigNumber(
                        destinationAddressAndValueArray[i].split(',')[1],
                    ),
                );
            }
        } else {
            // this is a one to one XEC transaction then check singleSendValue
            // note: one to many transactions won't be sending a singleSendValue param

            if (!singleSendValue) {
                throw new Error('Invalid singleSendValue');
            }

            value = new BigNumber(singleSendValue);
        }
        // If user is attempting to send an aggregate value that is less than minimum accepted by the backend
        if (
            value.lt(
                new BigNumber(
                    fromSmallestDenomination(currency.dustSats).toString(),
                ),
            )
        ) {
            // Throw the same error given by the backend attempting to broadcast such a tx
            throw new Error('dust');
        }
    } catch (err) {
        console.log('Error in parseXecSendValue: ' + err);
        throw err;
    }
    return value;
};

/*
 * Generates an OP_RETURN script to reflect the various send XEC permutations
 * involving messaging, encryption, eToken IDs and airdrop flags.
 *
 * Returns the final encoded script object
 */
export const generateOpReturnScript = (
    BCH,
    optionalOpReturnMsg,
    encryptionFlag,
    airdropFlag,
    airdropTokenId,
    encryptedEj,
) => {
    // encrypted mesage is mandatory when encryptionFlag is true
    // airdrop token id is mandatory when airdropFlag is true
    if (
        !BCH ||
        (encryptionFlag && !encryptedEj) ||
        (airdropFlag && !airdropTokenId)
    ) {
        throw new Error('Invalid OP RETURN script input');
    }

    // Note: script.push(Buffer.from(currency.opReturn.opReturnPrefixHex, 'hex')); actually evaluates to '016a'
    // instead of keeping the hex string intact. This behavour is specific to the initial script array element.
    // To get around this, the bch-js approach of directly using the opReturn prefix in decimal form for the initial entry is used here.
    let script = [currency.opReturn.opReturnPrefixDec]; // initialize script with the OP_RETURN op code (6a) in decimal form (106)

    try {
        if (encryptionFlag) {
            // if the user has opted to encrypt this message

            // add the encrypted cashtab messaging prefix and encrypted msg to script
            script.push(
                Buffer.from(
                    currency.opReturn.appPrefixesHex.cashtabEncrypted,
                    'hex',
                ), // 65746162
            );

            // add the encrypted message to script
            script.push(Buffer.from(encryptedEj));
        } else {
            // this is an un-encrypted message

            if (airdropFlag) {
                // if this was routed from the airdrop component
                // add the airdrop prefix to script
                script.push(
                    Buffer.from(
                        currency.opReturn.appPrefixesHex.airdrop,
                        'hex',
                    ), // drop
                );
                // add the airdrop token ID to script
                script.push(Buffer.from(airdropTokenId, 'hex'));
            }

            // add the cashtab prefix to script
            script.push(
                Buffer.from(currency.opReturn.appPrefixesHex.cashtab, 'hex'), // 00746162
            );

            // add the un-encrypted message to script if supplied
            if (optionalOpReturnMsg) {
                script.push(Buffer.from(optionalOpReturnMsg));
            }
        }
    } catch (err) {
        console.log('Error in generateOpReturnScript(): ' + err);
        throw err;
    }

    const data = BCH.Script.encode(script);
    return data;
};

export const generateTxOutput = (
    BCH,
    isOneToMany,
    singleSendValue,
    satoshisToSend,
    totalInputUtxoValue,
    destinationAddress,
    destinationAddressAndValueArray,
    changeAddress,
    txFee,
    txBuilder,
) => {
    try {
        if (
            !BCH ||
            (isOneToMany && !destinationAddressAndValueArray) ||
            (!isOneToMany && !destinationAddress && !singleSendValue) ||
            !changeAddress ||
            !satoshisToSend ||
            !totalInputUtxoValue ||
            !txFee ||
            !txBuilder
        ) {
            throw new Error('Invalid tx input parameter');
        }

        // amount to send back to the remainder address.
        const remainder = new BigNumber(totalInputUtxoValue)
            .minus(satoshisToSend)
            .minus(txFee);
        if (remainder.lt(0)) {
            throw new Error(`Insufficient funds`);
        }

        if (isOneToMany) {
            // for one to many mode, add the multiple outputs from the array
            let arrayLength = destinationAddressAndValueArray.length;
            for (let i = 0; i < arrayLength; i++) {
                // add each send tx from the array as an output
                let outputAddress =
                    destinationAddressAndValueArray[i].split(',')[0];
                let outputValue = new BigNumber(
                    destinationAddressAndValueArray[i].split(',')[1],
                );
                txBuilder.addOutput(
                    BCH.Address.toCashAddress(outputAddress),
                    parseInt(toSmallestDenomination(outputValue)),
                );
            }
        } else {
            // for one to one mode, add output w/ single address and amount to send
            txBuilder.addOutput(
                BCH.Address.toCashAddress(destinationAddress),
                parseInt(toSmallestDenomination(singleSendValue)),
            );
        }

        // if a remainder exists, return to change address as the final output
        if (remainder.gte(new BigNumber(currency.dustSats))) {
            txBuilder.addOutput(changeAddress, parseInt(remainder));
        }
    } catch (err) {
        console.log('Error in generateTxOutput(): ' + err);
        throw err;
    }

    return txBuilder;
};

export function parseOpReturn(hexStr) {
    if (
        !hexStr ||
        typeof hexStr !== 'string' ||
        hexStr.substring(0, 2) !== currency.opReturn.opReturnPrefixHex
    ) {
        return false;
    }

    hexStr = hexStr.slice(2); // remove the first byte i.e. 6a

    /*
     * @Return: resultArray is structured as follows:
     *  resultArray[0] is the transaction type i.e. eToken prefix, cashtab prefix, external message itself if unrecognized prefix
     *  resultArray[1] is the actual cashtab message or the 2nd part of an external message
     *  resultArray[2 - n] are the additional messages for future protcols
     */
    let resultArray = [];
    let message = '';
    let hexStrLength = hexStr.length;

    for (let i = 0; hexStrLength !== 0; i++) {
        // part 1: check the preceding byte value for the subsequent message
        let byteValue = hexStr.substring(0, 2);
        let msgByteSize = 0;
        if (byteValue === currency.opReturn.opPushDataOne) {
            // if this byte is 4c then the next byte is the message byte size - retrieve the message byte size only
            msgByteSize = parseInt(hexStr.substring(2, 4), 16); // hex base 16 to decimal base 10
            hexStr = hexStr.slice(4); // strip the 4c + message byte size info
        } else {
            // take the byte as the message byte size
            msgByteSize = parseInt(hexStr.substring(0, 2), 16); // hex base 16 to decimal base 10
            hexStr = hexStr.slice(2); // strip the message byte size info
        }

        // part 2: parse the subsequent message based on bytesize
        const msgCharLength = 2 * msgByteSize;
        message = hexStr.substring(0, msgCharLength);
        if (i === 0 && message === currency.opReturn.appPrefixesHex.eToken) {
            // add the extracted eToken prefix to array then exit loop
            resultArray[i] = currency.opReturn.appPrefixesHex.eToken;
            break;
        } else if (
            i === 0 &&
            message === currency.opReturn.appPrefixesHex.cashtab
        ) {
            // add the extracted Cashtab prefix to array
            resultArray[i] = currency.opReturn.appPrefixesHex.cashtab;
        } else if (
            i === 0 &&
            message === currency.opReturn.appPrefixesHex.cashtabEncrypted
        ) {
            // add the Cashtab encryption prefix to array
            resultArray[i] = currency.opReturn.appPrefixesHex.cashtabEncrypted;
        } else if (
            i === 0 &&
            message === currency.opReturn.appPrefixesHex.airdrop
        ) {
            // add the airdrop prefix to array
            resultArray[i] = currency.opReturn.appPrefixesHex.airdrop;
        } else {
            // this is either an external message or a subsequent cashtab message loop to extract the message
            resultArray[i] = message;
        }

        // strip out the parsed message
        hexStr = hexStr.slice(msgCharLength);
        hexStrLength = hexStr.length;
    }
    return resultArray;
}

export const fromLegacyDecimals = (
    amount,
    cashDecimals = currency.cashDecimals,
) => {
    // Input 0.00000546 BCH
    // Output 5.46 XEC or 0.00000546 BCH, depending on currency.cashDecimals
    const amountBig = new BigNumber(amount);
    const conversionFactor = new BigNumber(10 ** (8 - cashDecimals));
    const amountSmallestDenomination = amountBig
        .times(conversionFactor)
        .toNumber();
    return amountSmallestDenomination;
};

export const fromSmallestDenomination = (
    amount,
    cashDecimals = currency.cashDecimals,
) => {
    const amountBig = new BigNumber(amount);
    const multiplier = new BigNumber(10 ** (-1 * cashDecimals));
    const amountInBaseUnits = amountBig.times(multiplier);
    return amountInBaseUnits.toNumber();
};

export const toSmallestDenomination = (
    sendAmount,
    cashDecimals = currency.cashDecimals,
) => {
    // Replace the BCH.toSatoshi method with an equivalent function that works for arbitrary decimal places
    // Example, for an 8 decimal place currency like Bitcoin
    // Input: a BigNumber of the amount of Bitcoin to be sent
    // Output: a BigNumber of the amount of satoshis to be sent, or false if input is invalid

    // Validate
    // Input should be a BigNumber with no more decimal places than cashDecimals
    const isValidSendAmount =
        BigNumber.isBigNumber(sendAmount) && sendAmount.dp() <= cashDecimals;
    if (!isValidSendAmount) {
        return false;
    }
    const conversionFactor = new BigNumber(10 ** cashDecimals);
    const sendAmountSmallestDenomination = sendAmount.times(conversionFactor);
    return sendAmountSmallestDenomination;
};

export const batchArray = (inputArray, batchSize) => {
    // take an array of n elements, return an array of arrays each of length batchSize

    const batchedArray = [];
    for (let i = 0; i < inputArray.length; i += batchSize) {
        const tempArray = inputArray.slice(i, i + batchSize);
        batchedArray.push(tempArray);
    }
    return batchedArray;
};

export const flattenBatchedHydratedUtxos = batchedHydratedUtxoDetails => {
    // Return same result as if only the bulk API call were made
    // to do this, just need to move all utxos under one slpUtxos
    /*
    given 
    [
      {
        slpUtxos: [
            {
                utxos: [],
                address: '',
            }
          ],
      },
      {
        slpUtxos: [
            {
                utxos: [],
                address: '',
            }
          ],
      }
    ]
  return [
    {
        slpUtxos: [
            {
            utxos: [],
            address: ''
            },
            {
            utxos: [],
            address: ''
            },
          ]
        }
  */
    const flattenedBatchedHydratedUtxos = { slpUtxos: [] };
    for (let i = 0; i < batchedHydratedUtxoDetails.length; i += 1) {
        const theseSlpUtxos = batchedHydratedUtxoDetails[i].slpUtxos[0];
        flattenedBatchedHydratedUtxos.slpUtxos.push(theseSlpUtxos);
    }
    return flattenedBatchedHydratedUtxos;
};

export const flattenContactList = contactList => {
    /*
    Converts contactList from array of objects of type {address: <valid XEC address>, name: <string>} to array of addresses only

    If contact list is invalid, returns and empty array
    */
    if (!isValidContactList(contactList)) {
        return [];
    }
    let flattenedContactList = [];
    for (let i = 0; i < contactList.length; i += 1) {
        const thisAddress = contactList[i].address;
        flattenedContactList.push(thisAddress);
    }
    return flattenedContactList;
};

export const loadStoredWallet = walletStateFromStorage => {
    // Accept cached tokens array that does not save BigNumber type of BigNumbers
    // Return array with BigNumbers converted
    // See BigNumber.js api for how to create a BigNumber object from an object
    // https://mikemcl.github.io/bignumber.js/
    const liveWalletState = walletStateFromStorage;
    const { slpBalancesAndUtxos, tokens } = liveWalletState;
    for (let i = 0; i < tokens.length; i += 1) {
        const thisTokenBalance = tokens[i].balance;
        thisTokenBalance._isBigNumber = true;
        tokens[i].balance = new BigNumber(thisTokenBalance);
    }

    // Also confirm balance is correct
    // Necessary step in case currency.decimals changed since last startup
    const balancesRebased = normalizeBalance(slpBalancesAndUtxos);
    liveWalletState.balances = balancesRebased;
    return liveWalletState;
};

export const normalizeBalance = slpBalancesAndUtxos => {
    const totalBalanceInSatoshis = slpBalancesAndUtxos.nonSlpUtxos.reduce(
        (previousBalance, utxo) => previousBalance + utxo.value,
        0,
    );
    return {
        totalBalanceInSatoshis,
        totalBalance: fromSmallestDenomination(totalBalanceInSatoshis),
    };
};

export const isValidStoredWallet = walletStateFromStorage => {
    return (
        typeof walletStateFromStorage === 'object' &&
        'state' in walletStateFromStorage &&
        typeof walletStateFromStorage.state === 'object' &&
        'balances' in walletStateFromStorage.state &&
        'utxos' in walletStateFromStorage.state &&
        'hydratedUtxoDetails' in walletStateFromStorage.state &&
        'slpBalancesAndUtxos' in walletStateFromStorage.state &&
        'tokens' in walletStateFromStorage.state
    );
};

export const getWalletState = wallet => {
    if (!wallet || !wallet.state) {
        return {
            balances: { totalBalance: 0, totalBalanceInSatoshis: 0 },
            hydratedUtxoDetails: {},
            tokens: [],
            slpBalancesAndUtxos: {},
            parsedTxHistory: [],
            utxos: [],
        };
    }

    return wallet.state;
};

export function convertEtokenToEcashAddr(eTokenAddress) {
    if (!eTokenAddress) {
        return new Error(
            `cashMethods.convertToEcashAddr() error: No etoken address provided`,
        );
    }

    // Confirm input is a valid eToken address
    const isValidInput = isValidEtokenAddress(eTokenAddress);
    if (!isValidInput) {
        return new Error(
            `cashMethods.convertToEcashAddr() error: ${eTokenAddress} is not a valid etoken address`,
        );
    }

    // Check for etoken: prefix
    const isPrefixedEtokenAddress = eTokenAddress.slice(0, 7) === 'etoken:';

    // If no prefix, assume it is checksummed for an etoken: prefix
    const testedEtokenAddr = isPrefixedEtokenAddress
        ? eTokenAddress
        : `etoken:${eTokenAddress}`;

    let ecashAddress;
    try {
        const { type, hash } = cashaddr.decode(testedEtokenAddr);
        ecashAddress = cashaddr.encode('ecash', type, hash);
    } catch (err) {
        return err;
    }

    return ecashAddress;
}

export function convertToEcashPrefix(bitcoincashPrefixedAddress) {
    // Prefix-less addresses may be valid, but the cashaddr.decode function used below
    // will throw an error without a prefix. Hence, must ensure prefix to use that function.
    const hasPrefix = bitcoincashPrefixedAddress.includes(':');
    if (hasPrefix) {
        // Is it bitcoincash: or simpleledger:
        const { type, hash, prefix } = cashaddr.decode(
            bitcoincashPrefixedAddress,
        );

        let newPrefix;
        if (prefix === 'bitcoincash') {
            newPrefix = 'ecash';
        } else if (prefix === 'simpleledger') {
            newPrefix = 'etoken';
        } else {
            return bitcoincashPrefixedAddress;
        }

        const convertedAddress = cashaddr.encode(newPrefix, type, hash);

        return convertedAddress;
    } else {
        return bitcoincashPrefixedAddress;
    }
}

export function convertEcashtoEtokenAddr(eCashAddress) {
    const isValidInput = isValidXecAddress(eCashAddress);
    if (!isValidInput) {
        return new Error(`${eCashAddress} is not a valid ecash address`);
    }

    // Check for ecash: prefix
    const isPrefixedEcashAddress = eCashAddress.slice(0, 6) === 'ecash:';

    // If no prefix, assume it is checksummed for an ecash: prefix
    const testedEcashAddr = isPrefixedEcashAddress
        ? eCashAddress
        : `ecash:${eCashAddress}`;

    let eTokenAddress;
    try {
        const { type, hash } = cashaddr.decode(testedEcashAddr);
        eTokenAddress = cashaddr.encode('etoken', type, hash);
    } catch (err) {
        return new Error('eCash to eToken address conversion error');
    }
    return eTokenAddress;
}

export function toLegacyCash(addr) {
    // Confirm input is a valid ecash address
    const isValidInput = isValidXecAddress(addr);
    if (!isValidInput) {
        return new Error(`${addr} is not a valid ecash address`);
    }

    // Check for ecash: prefix
    const isPrefixedXecAddress = addr.slice(0, 6) === 'ecash:';

    // If no prefix, assume it is checksummed for an ecash: prefix
    const testedXecAddr = isPrefixedXecAddress ? addr : `ecash:${addr}`;

    let legacyCashAddress;
    try {
        const { type, hash } = cashaddr.decode(testedXecAddr);
        legacyCashAddress = cashaddr.encode(currency.legacyPrefix, type, hash);
    } catch (err) {
        return err;
    }
    return legacyCashAddress;
}

export function toLegacyCashArray(addressArray) {
    let cleanArray = []; // array of bch converted addresses to be returned

    if (
        addressArray === null ||
        addressArray === undefined ||
        !addressArray.length ||
        addressArray === ''
    ) {
        return new Error('Invalid addressArray input');
    }

    const arrayLength = addressArray.length;

    for (let i = 0; i < arrayLength; i++) {
        let addressValueArr = addressArray[i].split(',');
        let address = addressValueArr[0];
        let value = addressValueArr[1];

        // NB that toLegacyCash() includes address validation; will throw error for invalid address input
        const legacyAddress = toLegacyCash(address);
        if (legacyAddress instanceof Error) {
            return legacyAddress;
        }
        let convertedArrayData = legacyAddress + ',' + value + '\n';
        cleanArray.push(convertedArrayData);
    }

    return cleanArray;
}

export function toLegacyToken(addr) {
    // Confirm input is a valid ecash address
    const isValidInput = isValidEtokenAddress(addr);
    if (!isValidInput) {
        return new Error(`${addr} is not a valid etoken address`);
    }

    // Check for ecash: prefix
    const isPrefixedEtokenAddress = addr.slice(0, 7) === 'etoken:';

    // If no prefix, assume it is checksummed for an ecash: prefix
    const testedEtokenAddr = isPrefixedEtokenAddress ? addr : `etoken:${addr}`;

    let legacyTokenAddress;
    try {
        const { type, hash } = cashaddr.decode(testedEtokenAddr);
        legacyTokenAddress = cashaddr.encode('simpleledger', type, hash);
    } catch (err) {
        return err;
    }
    return legacyTokenAddress;
}

export const confirmNonEtokenUtxos = (hydratedUtxos, nonEtokenUtxos) => {
    // scan through hydratedUtxoDetails
    for (let i = 0; i < hydratedUtxos.length; i += 1) {
        // Find utxos with txids matching nonEtokenUtxos
        if (nonEtokenUtxos.includes(hydratedUtxos[i].txid)) {
            // Confirm that such utxos are not eToken utxos
            hydratedUtxos[i].isValid = false;
        }
    }
    return hydratedUtxos;
};

export const checkNullUtxosForTokenStatus = txDataResults => {
    const nonEtokenUtxos = [];
    for (let j = 0; j < txDataResults.length; j += 1) {
        const thisUtxoTxid = txDataResults[j].txid;
        const thisUtxoVout = txDataResults[j].details.vout;
        // Iterate over outputs
        for (let k = 0; k < thisUtxoVout.length; k += 1) {
            const thisOutput = thisUtxoVout[k];
            if (thisOutput.scriptPubKey.type === 'nulldata') {
                const asmOutput = thisOutput.scriptPubKey.asm;
                if (asmOutput.includes('OP_RETURN 5262419')) {
                    // then it's an eToken tx that has not been properly validated
                    // Do not include it in nonEtokenUtxos
                    // App will ignore it until SLPDB is able to validate it
                    /*
                    console.log(
                        `utxo ${thisUtxoTxid} requires further eToken validation, ignoring`,
                    );*/
                } else {
                    // Otherwise it's just an OP_RETURN tx that SLPDB has some issue with
                    // It should still be in the user's utxo set
                    // Include it in nonEtokenUtxos
                    /*
                    console.log(
                        `utxo ${thisUtxoTxid} is not an eToken tx, adding to nonSlpUtxos`,
                    );
                    */
                    nonEtokenUtxos.push(thisUtxoTxid);
                }
            }
        }
    }
    return nonEtokenUtxos;
};

/* Converts a serialized buffer containing encrypted data into an object
 * that can be interpreted by the ecies-lite library.
 *
 * For reference on the parsing logic in this function refer to the link below on the segment of
 * ecies-lite's encryption function where the encKey, macKey, iv and cipher are sliced and concatenated
 * https://github.com/tibetty/ecies-lite/blob/8fd97e80b443422269d0223ead55802378521679/index.js#L46-L55
 *
 * A similar PSF implmentation can also be found at:
 * https://github.com/Permissionless-Software-Foundation/bch-encrypt-lib/blob/master/lib/encryption.js
 *
 * For more detailed overview on the ecies encryption scheme, see https://cryptobook.nakov.com/asymmetric-key-ciphers/ecies-public-key-encryption
 */
export const convertToEncryptStruct = encryptionBuffer => {
    // based on ecies-lite's encryption logic, the encryption buffer is concatenated as follows:
    //  [ epk + iv + ct + mac ]  whereby:
    // - The first 32 or 64 chars of the encryptionBuffer is the epk
    // - Both iv and ct params are 16 chars each, hence their combined substring is 32 chars from the end of the epk string
    //    - within this combined iv/ct substring, the first 16 chars is the iv param, and ct param being the later half
    // - The mac param is appended to the end of the encryption buffer

    // validate input buffer
    if (!encryptionBuffer) {
        throw new Error(
            'cashmethods.convertToEncryptStruct() error: input must be a buffer',
        );
    }

    try {
        // variable tracking the starting char position for string extraction purposes
        let startOfBuf = 0;

        // *** epk param extraction ***
        // The first char of the encryptionBuffer indicates the type of the public key
        // If the first char is 4, then the public key is 64 chars
        // If the first char is 3 or 2, then the public key is 32 chars
        // Otherwise this is not a valid encryption buffer compatible with the ecies-lite library
        let publicKey;
        switch (encryptionBuffer[0]) {
            case 4:
                publicKey = encryptionBuffer.slice(0, 65); //  extract first 64 chars as public key
                break;
            case 3:
            case 2:
                publicKey = encryptionBuffer.slice(0, 33); //  extract first 32 chars as public key
                break;
            default:
                throw new Error(`Invalid type: ${encryptionBuffer[0]}`);
        }

        // *** iv and ct param extraction ***
        startOfBuf += publicKey.length; // sets the starting char position to the end of the public key (epk) in order to extract subsequent iv and ct substrings
        const encryptionTagLength = 32; // the length of the encryption tag (i.e. mac param) computed from each block of ciphertext, and is used to verify no one has tampered with the encrypted data
        const ivCtSubstring = encryptionBuffer.slice(
            startOfBuf,
            encryptionBuffer.length - encryptionTagLength,
        ); // extract the substring containing both iv and ct params, which is after the public key but before the mac param i.e. the 'encryption tag'
        const ivbufParam = ivCtSubstring.slice(0, 16); // extract the first 16 chars of substring as the iv param
        const ctbufParam = ivCtSubstring.slice(16); // extract the last 16 chars as substring the ct param

        // *** mac param extraction ***
        const macParam = encryptionBuffer.slice(
            encryptionBuffer.length - encryptionTagLength,
            encryptionBuffer.length,
        ); // extract the mac param appended to the end of the buffer

        return {
            iv: ivbufParam,
            epk: publicKey,
            ct: ctbufParam,
            mac: macParam,
        };
    } catch (err) {
        console.error(`useBCH.convertToEncryptStruct() error: `, err);
        throw err;
    }
};

export const getPublicKey = async (BCH, address) => {
    try {
        const publicKey = await BCH.encryption.getPubKey(address);
        return publicKey.publicKey;
    } catch (err) {
        if (err['error'] === 'No transaction history.') {
            throw new Error(
                'Cannot send an encrypted message to a wallet with no outgoing transactions',
            );
        } else {
            throw err;
        }
    }
};

export const isLegacyMigrationRequired = wallet => {
    // If the wallet does not have Path1899,
    // Or each Path1899, Path145, Path245 does not have a public key
    // Then it requires migration
    if (
        !wallet.Path1899 ||
        !wallet.Path1899.publicKey ||
        !wallet.Path1899.hash160 ||
        !wallet.Path145.publicKey ||
        !wallet.Path145.hash160 ||
        !wallet.Path245.publicKey ||
        !wallet.Path245.hash160
    ) {
        return true;
    }

    return false;
};

export const isExcludedUtxo = (utxo, utxoArray) => {
    /*
    utxo is a single utxo of model
    {
        height: 724992
        tx_hash: "8d4bdedb7c4443412e0c2f316a330863aef54d9ba73560ca60cca6408527b247"
        tx_pos: 0
        value: 10200
    }

    utxoArray is an array of utxos
    */
    let isExcludedUtxo = true;
    const { tx_hash, tx_pos, value } = utxo;
    for (let i = 0; i < utxoArray.length; i += 1) {
        const thisUtxo = utxoArray[i];
        // NOTE
        // You can't match height, as this changes from 0 to blockheight after confirmation
        //const thisUtxoHeight = thisUtxo.height;
        const thisUtxoTxid = thisUtxo.tx_hash;
        const thisUtxoTxPos = thisUtxo.tx_pos;
        const thisUtxoValue = thisUtxo.value;
        // If you find a utxo such that each object key is identical
        if (
            tx_hash === thisUtxoTxid &&
            tx_pos === thisUtxoTxPos &&
            value === thisUtxoValue
        ) {
            // Then this utxo is not excluded from the array
            isExcludedUtxo = false;
        }
    }

    return isExcludedUtxo;
};

export const whichUtxosWereAdded = (previousUtxos, currentUtxos) => {
    let utxosAddedFlag = false;
    const utxosAdded = [];

    // Iterate over currentUtxos
    // For each currentUtxo -- does it exist in previousUtxos?
    // If no, it's added

    // Note that the inputs are arrays of arrays, model
    /*
    previousUtxos = [{address: 'string', utxos: []}, ...]
    */

    // Iterate over the currentUtxos array of {address: 'string', utxos: []} objects
    for (let i = 0; i < currentUtxos.length; i += 1) {
        // Take the first object
        const thisCurrentUtxoObject = currentUtxos[i];
        const thisCurrentUtxoObjectAddress = thisCurrentUtxoObject.address;
        const thisCurrentUtxoObjectUtxos = thisCurrentUtxoObject.utxos;
        // Iterate over the previousUtxos array of {address: 'string', utxos: []} objects
        for (let j = 0; j < previousUtxos.length; j += 1) {
            const thisPreviousUtxoObject = previousUtxos[j];
            const thisPreviousUtxoObjectAddress =
                thisPreviousUtxoObject.address;
            // When you find the utxos object at the same address
            if (
                thisCurrentUtxoObjectAddress === thisPreviousUtxoObjectAddress
            ) {
                // Create a utxosAddedObject with the address
                const utxosAddedObject = {
                    address: thisCurrentUtxoObjectAddress,
                    utxos: [],
                };
                utxosAdded.push(utxosAddedObject);

                // Grab the previousUtxoObject utxos array. thisCurrentUtxoObjectUtxos has changed compared to thisPreviousUtxoObjectUtxos
                const thisPreviousUtxoObjectUtxos =
                    thisPreviousUtxoObject.utxos;
                // To see if any utxos exist in thisCurrentUtxoObjectUtxos that do not exist in thisPreviousUtxoObjectUtxos
                // iterate over thisPreviousUtxoObjectUtxos for each utxo in thisCurrentUtxoObjectUtxos
                for (let k = 0; k < thisCurrentUtxoObjectUtxos.length; k += 1) {
                    const thisCurrentUtxo = thisCurrentUtxoObjectUtxos[k];

                    if (
                        isExcludedUtxo(
                            thisCurrentUtxo,
                            thisPreviousUtxoObjectUtxos,
                        )
                    ) {
                        // If thisCurrentUtxo was not in the corresponding previous utxos
                        // Then it was added
                        utxosAdded[j].utxos.push(thisCurrentUtxo);
                        utxosAddedFlag = true;
                    }
                }
            }
        }
    }
    // If utxos were added, return them
    if (utxosAddedFlag) {
        return utxosAdded;
    }
    // Else return false
    return utxosAddedFlag;
};

export const whichUtxosWereConsumed = (previousUtxos, currentUtxos) => {
    let utxosConsumedFlag = false;
    const utxosConsumed = [];
    // Iterate over previousUtxos
    // For each previousUtxo -- does it exist in currentUtxos?
    // If no, it's consumed

    // Note that the inputs are arrays of arrays, model
    /*
    previousUtxos = [{address: 'string', utxos: []}, ...]
    */

    // Iterate over the previousUtxos array of {address: 'string', utxos: []} objects
    for (let i = 0; i < previousUtxos.length; i += 1) {
        // Take the first object
        const thisPreviousUtxoObject = previousUtxos[i];
        const thisPreviousUtxoObjectAddress = thisPreviousUtxoObject.address;
        const thisPreviousUtxoObjectUtxos = thisPreviousUtxoObject.utxos;
        // Iterate over the currentUtxos array of {address: 'string', utxos: []} objects
        for (let j = 0; j < currentUtxos.length; j += 1) {
            const thisCurrentUtxoObject = currentUtxos[j];
            const thisCurrentUtxoObjectAddress = thisCurrentUtxoObject.address;
            // When you find the utxos object at the same address
            if (
                thisCurrentUtxoObjectAddress === thisPreviousUtxoObjectAddress
            ) {
                // Create a utxosConsumedObject with the address
                const utxosConsumedObject = {
                    address: thisCurrentUtxoObjectAddress,
                    utxos: [],
                };
                utxosConsumed.push(utxosConsumedObject);
                // Grab the currentUtxoObject utxos array. thisCurrentUtxoObjectUtxos has changed compared to thisPreviousUtxoObjectUtxos
                const thisCurrentUtxoObjectUtxos = thisCurrentUtxoObject.utxos;
                // To see if any utxos exist in thisPreviousUtxoObjectUtxos that do not exist in thisCurrentUtxoObjectUtxos
                // iterate over thisCurrentUtxoObjectUtxos for each utxo in thisPreviousUtxoObjectUtxos
                for (
                    let k = 0;
                    k < thisPreviousUtxoObjectUtxos.length;
                    k += 1
                ) {
                    const thisPreviousUtxo = thisPreviousUtxoObjectUtxos[k];
                    // If thisPreviousUtxo was not in the corresponding current utxos

                    if (
                        isExcludedUtxo(
                            thisPreviousUtxo,
                            thisCurrentUtxoObjectUtxos,
                        )
                    ) {
                        // Then it was consumed
                        utxosConsumed[j].utxos.push(thisPreviousUtxo);
                        utxosConsumedFlag = true;
                    }
                }
            }
        }
    }
    // If utxos were consumed, return them
    if (utxosConsumedFlag) {
        return utxosConsumed;
    }
    // Else return false
    return utxosConsumedFlag;
};

export const addNewHydratedUtxos = (
    addedHydratedUtxos,
    hydratedUtxoDetails,
) => {
    const theseAdditionalHydratedUtxos = addedHydratedUtxos.slpUtxos;
    for (let i = 0; i < theseAdditionalHydratedUtxos.length; i += 1) {
        const thisHydratedUtxoObj = theseAdditionalHydratedUtxos[i];
        hydratedUtxoDetails.slpUtxos.push(thisHydratedUtxoObj);
    }
    return hydratedUtxoDetails;
    // Add hydrateUtxos(addedUtxos) to hydratedUtxoDetails
    /*
    e.g. add this
    {
    "slpUtxos": 
        [
            {
                "utxos": [
                    {
                        "height": 725886,
                        "tx_hash": "29985c01444bf80ade764e5d40d7ec2c12317e03301243170139c75f20c51f78",
                        "tx_pos": 0,
                        "value": 3300,
                        "txid": "29985c01444bf80ade764e5d40d7ec2c12317e03301243170139c75f20c51f78",
                        "vout": 0,
                        "isValid": false
                    }
                ],
                "address": "bitcoincash:qz2708636snqhsxu8wnlka78h6fdp77ar5ulhz04hr"
            }
        ]
    }

to this

{
    "slpUtxos": 
        [
            {
                "utxos": [
                    {
                        "height": 725886,
                        "tx_hash": "29985c01444bf80ade764e5d40d7ec2c12317e03301243170139c75f20c51f78",
                        "tx_pos": 0,
                        "value": 3300,
                        "txid": "29985c01444bf80ade764e5d40d7ec2c12317e03301243170139c75f20c51f78",
                        "vout": 0,
                        "isValid": false
                    }
                    ... up to 20
                ],
                "address": "bitcoincash:qz2708636snqhsxu8wnlka78h6fdp77ar5ulhz04hr"
            },
            {
                "utxos": [
                    {
                        "height": 725886,
                        "tx_hash": "29985c01444bf80ade764e5d40d7ec2c12317e03301243170139c75f20c51f78",
                        "tx_pos": 0,
                        "value": 3300,
                        "txid": "29985c01444bf80ade764e5d40d7ec2c12317e03301243170139c75f20c51f78",
                        "vout": 0,
                        "isValid": false
                    }
                    ... up to 20
                ],
                "address": "bitcoincash:qz2708636snqhsxu8wnlka78h6fdp77ar5ulhz04hr"
            }
            ,
            ... a bunch of these in batches of 20
        ]
    }
    */
};

export const removeConsumedUtxos = (consumedUtxos, hydratedUtxoDetails) => {
    let hydratedUtxoDetailsWithConsumedUtxosRemoved = hydratedUtxoDetails;
    const slpUtxosArray = hydratedUtxoDetails.slpUtxos;
    // Iterate over consumedUtxos
    // Every utxo in consumedUtxos must be removed from hydratedUtxoDetails
    for (let i = 0; i < consumedUtxos.length; i += 1) {
        const thisConsumedUtxoObject = consumedUtxos[i]; // {address: 'string', utxos: [{},{},...{}]}
        const thisConsumedUtxoObjectAddr = thisConsumedUtxoObject.address;
        const thisConsumedUtxoObjectUtxoArray = thisConsumedUtxoObject.utxos;
        for (let j = 0; j < thisConsumedUtxoObjectUtxoArray.length; j += 1) {
            const thisConsumedUtxo = thisConsumedUtxoObjectUtxoArray[j];
            // Iterate through slpUtxosArray to find thisConsumedUtxo
            slpUtxosArrayLoop: for (
                let k = 0;
                k < slpUtxosArray.length;
                k += 1
            ) {
                const thisSlpUtxosArrayUtxoObject = slpUtxosArray[k]; // {address: 'string', utxos: [{},{},...{}]}
                const thisSlpUtxosArrayUtxoObjectAddr =
                    thisSlpUtxosArrayUtxoObject.address;
                // If this address matches the address of the consumed utxo, check for a consumedUtxo match
                // Note, slpUtxos may have many utxo objects with the same address, need to check them all until you find and remove this consumed utxo
                if (
                    thisConsumedUtxoObjectAddr ===
                    thisSlpUtxosArrayUtxoObjectAddr
                ) {
                    const thisSlpUtxosArrayUtxoObjectUtxoArray =
                        thisSlpUtxosArrayUtxoObject.utxos;

                    // Iterate to find it and remove it
                    for (
                        let m = 0;
                        m < thisSlpUtxosArrayUtxoObjectUtxoArray.length;
                        m += 1
                    ) {
                        const thisHydratedUtxo =
                            thisSlpUtxosArrayUtxoObjectUtxoArray[m];
                        if (
                            thisConsumedUtxo.tx_hash ===
                                thisHydratedUtxo.tx_hash &&
                            thisConsumedUtxo.tx_pos ===
                                thisHydratedUtxo.tx_pos &&
                            thisConsumedUtxo.value === thisHydratedUtxo.value
                        ) {
                            // remove it
                            hydratedUtxoDetailsWithConsumedUtxosRemoved.slpUtxos[
                                k
                            ].utxos.splice(m, 1);
                            // go to the next consumedUtxo
                            break slpUtxosArrayLoop;
                        }
                    }
                }
            }
        }
    }
    return hydratedUtxoDetailsWithConsumedUtxosRemoved;
};

export const getUtxoCount = utxos => {
    // return how many utxos
    // return false if input is invalid
    /*
    Both utxos and hydratedUtxoDetails.slpUtxos are build like so
    [
        {
            address: 'string',
            utxos: [{}, {}, {}...{}]
        },
        {
            address: 'string',
            utxos: [{}, {}, {}...{}]
        },
        {
            address: 'string',
            utxos: [{}, {}, {}...{}]
        },
    ]

    We want a function that quickly determines how many utxos are here
    */

    // First, validate that you are getting a valid bch-api utxo set
    // if you are not, then return false -- which would cause areAllUtxosIncludedInIncrementallyHydratedUtxos to return false and calculate utxo set the legacy way
    const isValidUtxoObject = isValidBchApiUtxoObject(utxos);
    if (!isValidUtxoObject) {
        return false;
    }

    let utxoCount = 0;
    for (let i = 0; i < utxos.length; i += 1) {
        const thisUtxoArrLength = utxos[i].utxos.length;
        utxoCount += thisUtxoArrLength;
    }
    return utxoCount;
};

export const areAllUtxosIncludedInIncrementallyHydratedUtxos = (
    utxos,
    incrementallyHydratedUtxos,
) => {
    let incrementallyHydratedUtxosIncludesAllUtxosInLatestUtxoApiFetch = false;
    // check
    const { slpUtxos } = incrementallyHydratedUtxos;

    // Iterate over utxos array
    for (let i = 0; i < utxos.length; i += 1) {
        const thisUtxoObject = utxos[i];
        const thisUtxoObjectAddr = thisUtxoObject.address;
        const thisUtxoObjectUtxos = thisUtxoObject.utxos;
        let utxoFound;
        for (let j = 0; j < thisUtxoObjectUtxos.length; j += 1) {
            const thisUtxo = thisUtxoObjectUtxos[j];
            utxoFound = false;
            // Now iterate over slpUtxos to find it
            slpUtxosLoop: for (let k = 0; k < slpUtxos.length; k += 1) {
                const thisSlpUtxosObject = slpUtxos[k];
                const thisSlpUtxosObjectAddr = thisSlpUtxosObject.address;
                if (thisUtxoObjectAddr === thisSlpUtxosObjectAddr) {
                    const thisSlpUtxosObjectUtxos = thisSlpUtxosObject.utxos;
                    for (
                        let m = 0;
                        m < thisSlpUtxosObjectUtxos.length;
                        m += 1
                    ) {
                        const thisSlpUtxo = thisSlpUtxosObjectUtxos[m];
                        if (
                            thisUtxo.tx_hash === thisSlpUtxo.tx_hash &&
                            thisUtxo.tx_pos === thisSlpUtxo.tx_pos &&
                            thisUtxo.value === thisSlpUtxo.value
                        ) {
                            utxoFound = true;
                            // goto next utxo
                            break slpUtxosLoop;
                        }
                    }
                }
                if (k === slpUtxos.length - 1 && !utxoFound) {
                    // return false
                    return incrementallyHydratedUtxosIncludesAllUtxosInLatestUtxoApiFetch;
                }
            }
        }
    }
    // It's possible that hydratedUtxoDetails includes every utxo from the utxos array, but for some reason also includes additional utxos
    const utxosInUtxos = getUtxoCount(utxos);
    const utxosInIncrementallyHydratedUtxos = getUtxoCount(slpUtxos);
    if (
        !utxosInUtxos ||
        !utxosInIncrementallyHydratedUtxos ||
        utxosInUtxos !== utxosInIncrementallyHydratedUtxos
    ) {
        return incrementallyHydratedUtxosIncludesAllUtxosInLatestUtxoApiFetch;
    }
    // If you make it here, good to go
    incrementallyHydratedUtxosIncludesAllUtxosInLatestUtxoApiFetch = true;
    return incrementallyHydratedUtxosIncludesAllUtxosInLatestUtxoApiFetch;
};

export const getHashArrayFromWallet = wallet => {
    // If the wallet has wallet.Path1899.hash160, it's migrated and will have all of them
    // Return false for an umigrated wallet
    const hash160Array =
        wallet && wallet.Path1899 && 'hash160' in wallet.Path1899
            ? [
                  wallet.Path245.hash160,
                  wallet.Path145.hash160,
                  wallet.Path1899.hash160,
              ]
            : false;
    return hash160Array;
};

export const parseChronikTx = (tx, walletHash160s) => {
    const { inputs, outputs } = tx;
    // Assign defaults
    let incoming = true;
    let xecAmount = new BigNumber(0);
    let etokenAmount = new BigNumber(0);
    const isEtokenTx = 'slpTxData' in tx && typeof tx.slpTxData !== 'undefined';

    // Iterate over inputs to see if this is an incoming tx (incoming === true)
    for (let i = 0; i < inputs.length; i += 1) {
        const thisInput = inputs[i];
        const thisInputSendingHash160 = thisInput.outputScript;
        for (let j = 0; j < walletHash160s.length; j += 1) {
            const thisWalletHash160 = walletHash160s[j];
            if (thisInputSendingHash160.includes(thisWalletHash160)) {
                // Then this is an outgoing tx
                incoming = false;
                // Break out of this for loop once you know this is an incoming tx
                break;
            }
        }
    }
    // Iterate over outputs to get the amount sent
    for (let i = 0; i < outputs.length; i += 1) {
        const thisOutput = outputs[i];
        const thisOutputReceivedAtHash160 = thisOutput.outputScript;
        // Find amounts at your wallet's addresses
        for (let j = 0; j < walletHash160s.length; j += 1) {
            const thisWalletHash160 = walletHash160s[j];
            if (thisOutputReceivedAtHash160.includes(thisWalletHash160)) {
                // If incoming tx, this is amount received by the user's wallet
                // if outgoing tx (incoming === false), then this is a change amount
                const thisOutputAmount = new BigNumber(thisOutput.value);
                xecAmount = incoming
                    ? xecAmount.plus(thisOutputAmount)
                    : xecAmount.minus(thisOutputAmount);

                // Parse token qty if token tx
                // Note: edge case this is a token tx that sends XEC to Cashtab recipient but token somewhere else
                if (isEtokenTx) {
                    try {
                        const thisEtokenAmount = new BigNumber(
                            thisOutput.slpToken.amount,
                        );

                        etokenAmount = incoming
                            ? etokenAmount.plus(thisEtokenAmount)
                            : etokenAmount.minus(thisEtokenAmount);
                    } catch (err) {
                        // edge case described above; in this case there is zero eToken value for this Cashtab recipient, so add 0
                        etokenAmount.plus(new BigNumber(0));
                    }
                }
            }
        }
        // Output amounts not at your wallet are sent amounts if !incoming
        if (!incoming) {
            const thisOutputAmount = new BigNumber(thisOutput.value);
            xecAmount = xecAmount.plus(thisOutputAmount);
            if (isEtokenTx) {
                try {
                    const thisEtokenAmount = new BigNumber(
                        thisOutput.slpToken.amount,
                    );
                    etokenAmount = etokenAmount.plus(thisEtokenAmount);
                } catch (err) {
                    // NB the edge case described above cannot exist in an outgoing tx
                    // because the eTokens sent originated from this wallet
                }
            }
        }
    }

    // Convert from sats to XEC
    xecAmount = xecAmount.shiftedBy(-1 * currency.cashDecimals);

    // Convert from BigNumber to string
    xecAmount = xecAmount.toString();
    etokenAmount = etokenAmount.toString();

    // Return eToken specific fields if eToken tx
    if (isEtokenTx) {
        const { slpMeta } = tx.slpTxData;
        return {
            incoming,
            xecAmount,
            isEtokenTx,
            etokenAmount,
            slpMeta,
        };
    }
    // Otherwise do not include these fields
    return { incoming, xecAmount, isEtokenTx };
};

export const checkWalletForTokenInfo = (tokenId, wallet) => {
    /* 
    Check wallet for cached information about a given tokenId
    Return {decimals: tokenDecimals, name: tokenName, ticker: tokenTicker}
    If this tokenId does not exist in wallet, return false
    */
    try {
        const { tokens } = wallet.state;
        for (let i = 0; i < tokens.length; i += 1) {
            const thisTokenId = tokens[i].tokenId;
            if (tokenId === thisTokenId) {
                return {
                    decimals: tokens[i].info.decimals,
                    ticker: tokens[i].info.tokenTicker,
                    name: tokens[i].info.tokenName,
                };
            }
        }
    } catch (err) {
        return false;
    }

    return false;
};

export const isActiveWebsocket = ws => {
    // Return true if websocket is connected and subscribed
    // Otherwise return false
    return (
        ws !== null &&
        ws &&
        '_ws' in ws &&
        'readyState' in ws._ws &&
        ws._ws.readyState === 1 &&
        '_subs' in ws &&
        ws._subs.length > 0
    );
};
