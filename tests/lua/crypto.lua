local crypto=require('openssl')
local v=require('vectis')
local fixtures=assert(os.getenv('VECTIS_CRYPTO_FIXTURES'))
local root=assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local function read(path)
  local file=assert(io.open(path,'rb'))
  local data=assert(file:read('a')); assert(file:close()); return data
end
local options={private_key_pem=read(fixtures..'/private.pem')}
local key=assert(crypto.load_key(options))
options.private_key_pem='changed after loading'
local public=assert(crypto.load_key({public_key_path=fixtures..'/public.pem'}))
local certificate=assert(crypto.load_key({certificate_pem=read(fixtures..'/certificate.pem')}))
local encrypted=assert(crypto.load_key({private_key_path=fixtures..'/encrypted.pem',passphrase='fixture-password'}))
assert(key:info().type=='RSA' and key:info().bits==2048 and key:info().private_key)
assert(not public:info().private_key and public:info().max_signature_bytes==256)
local data='signed\0payload'
local signature=assert(key:sign(data))
assert(#signature==256 and public:verify(data,signature) and certificate:verify(data,signature))
assert(public:verify(data..'changed',signature)==false)
assert(public:sign(data)==nil)
local sha512=assert(encrypted:sign('',{digest='sha512'}))
assert(public:verify('',sha512,{algorithm='sha512'}))
assert(key:sign(data,{algorithm='unknown-digest'})==nil)
assert(public:verify(data,assert(key:sign(data))))
assert(crypto.verify({certificate_path=fixtures..'/certificate.pem',data=data,signature=signature}))
assert(certificate:verify(data,crypto.sign({private_key_path=fixtures..'/private.pem',data=data})))
assert(crypto.load_key({private_key_path=fixtures..'/encrypted.pem'})==nil)
assert(crypto.load_key({private_key_path=fixtures..'/encrypted.pem',passphrase='wrong'})==nil)
assert(crypto.load_key({private_key_pem='invalid'})==nil)
assert(crypto.load_key({private_key_path=root..'/missing-key'})==nil)
for _,callback in ipairs({
  function() crypto.load_key({}) end,
  function() crypto.load_key({private_key_path='a',public_key_path='b'}) end,
  function() crypto.load_key({private_key_path='a\0b'}) end,
  function() crypto.load_key({unknown=true}) end,
  function() key:sign(data,{unknown=true}) end,
  function() crypto.aes_gcm_encrypt_materialized({key='k',nonce='n',data='',unknown=true}) end,
}) do assert(not pcall(callback)) end
-- Existing certificate generation feeds the reusable-key workflow.
local generated_key,generated_cert=root..'/generated-key.pem',root..'/generated-cert.pem'
assert(v.cert.generate_bundle({common_name='owned-key.local',output_key_path=generated_key,output_cert_path=generated_cert,key_bits=2048,valid_days=1}))
local generated <close> = assert(crypto.load_key({private_key_path=generated_key}))
local generated_public <close> = assert(crypto.load_key({certificate_path=generated_cert}))
assert(generated_public:verify(data,assert(generated:sign(data))))
assert(key:close() and key:close() and public:close() and certificate:close() and encrypted:close())
assert(not pcall(function() key:info() end))
local zero_key,zero_nonce=string.rep('\0',16),string.rep('\0',12)
local vector=assert(crypto.aes_gcm_encrypt_materialized({key=zero_key,nonce=zero_nonce,data=string.rep('\0',16)}))
assert(crypto.hex_encode(vector.ciphertext)=='0388dace60b6a392f328c2b971b2fe78')
assert(crypto.hex_encode(vector.tag)=='ab6e47d42cec13bdf53a67b21257bddf')
for _,size in ipairs({16,24,32}) do
  local secret=string.rep('k',size)
  local nonce=string.rep('n',12)
  for _,plaintext in ipairs({'','binary\0plaintext'}) do
    local result=assert(crypto.aes_gcm_encrypt_materialized({key=secret,nonce=nonce,data=plaintext,aad='context\0'}))
    local input={key=secret,nonce=nonce,ciphertext=result.ciphertext,tag=result.tag,aad='context\0'}
    assert(crypto.aes_gcm_decrypt_materialized(input)==plaintext)
    for _,change in ipairs({{'key',secret:sub(1,-2)..'z'},{'nonce',nonce:sub(1,-2)..'z'},{'tag',string.rep('x',16)},{'aad','changed'}}) do
      local field,before=change[1],input[change[1]]
      input[field]=change[2]
      local failed,err=crypto.aes_gcm_decrypt_materialized(input)
      assert(failed==nil and err.operation=='aes_gcm_decrypt_materialized' and err.status~=0)
      input[field]=before
    end
  end
end
assert(crypto.aes_gcm_encrypt_materialized({key='short',nonce=zero_nonce,data=''})==nil)
assert(crypto.aes_gcm_encrypt_materialized({key=zero_key,nonce='short',data=''})==nil)
assert(crypto.aes_gcm_decrypt_materialized({key=zero_key,nonce=zero_nonce,ciphertext='',tag='short'})==nil)
collectgarbage('collect')
print('upgrade seam behavior ok')
