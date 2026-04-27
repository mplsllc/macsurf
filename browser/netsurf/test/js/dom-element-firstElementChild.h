<html>
<head>
<title>DOM firstElementChild reference (title)</title>
<link rel="stylesheet" type="text/css" href="tst.css">
</head>
<body>
<h1>DOM firstElementChild reference (body)</h1>
<noscript><p>Javascript is disabled</p></noscript>
<p><b>head.firstElementChild:</b> <script>document.write(document.head.firstElementChild.textContent);</script></p>
<p><b>body.firstElementChild:</b> <script>document.write(document.body.firstElementChild.textContent);</script></p>
<h1>DOM nodes should be 'null' if not present</h1>
<p><b>head.firstElementChild.firstElementChild:</b> <script>document.write(document.head.firstElementChild.firstElementChild);</script></p>
</body>
</html>
